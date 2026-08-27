package bytecode

import (
	"bytes"
	"encoding/binary"
	"fmt"

	"github.com/vx-lang/vxc/internal/typeir"
)

const (
	// MAGIC is the VXOBJ file magic bytes.
	// VXOBJ is a pure code-logic line binary representation: it stores only
	// TypeIR instructions stripped of all high-level features (generics,
	// traits, ownership annotations) and platform API calls. All platform
	// adaptation is deferred to the translator, which resolves abstract
	// operations against the target OS at translation time.
	//
	// This design ensures zero hidden behavior: VXOBJ contains exactly the
	// program's algorithmic logic, nothing more. The translator's output
	// (LLVM IR + platform bindings) is fully inspectable with -S.
	MAGIC             = "VXOBJ"
	VERSION_V4 uint32 = 4

	// Section names
	SECTION_TYPE_IR       = "TypeIR"
	SECTION_CODE          = "Code"
	SECTION_DEBUG         = "Debug"
	SECTION_SOURCE_MAP    = "SourceMap"
	SECTION_EXTERNAL_DEPS = "ExternalDeps"
)

type VxObjV4SectionIndex struct {
	Name   string
	Offset uint32
	Size   uint32
}

type VxObjV4Header struct {
	Version      uint32
	Flags        uint32
	TargetTriple string
	Sections     []VxObjV4SectionIndex
}

type VxObjV4Container struct {
	Header      VxObjV4Header
	SectionData map[string][]byte
}

func NewVxObjV4Container(targetTriple string) *VxObjV4Container {
	return &VxObjV4Container{
		Header: VxObjV4Header{
			Version:      VERSION_V4,
			Flags:        0,
			TargetTriple: targetTriple,
			Sections:     []VxObjV4SectionIndex{},
		},
		SectionData: make(map[string][]byte),
	}
}

func (c *VxObjV4Container) SetSection(name string, data []byte) {
	c.SectionData[name] = data
	c.Header.Sections = append(c.Header.Sections, VxObjV4SectionIndex{
		Name:   name,
		Offset: 0,
		Size:   uint32(len(data)),
	})
}

func (c *VxObjV4Container) GetSection(name string) []byte {
	return c.SectionData[name]
}

func (c *VxObjV4Container) HasExternalDeps() bool {
	return (c.Header.Flags & 1) != 0
}

func (c *VxObjV4Container) SetExternalDepsFlag(hasDeps bool) {
	if hasDeps {
		c.Header.Flags |= 1
	} else {
		c.Header.Flags &= ^uint32(1)
	}
}

func (c *VxObjV4Container) Write(writer *bytes.Buffer) error {
	var sections []struct {
		Name string
		Data []byte
	}
	for _, sec := range c.Header.Sections {
		if data, ok := c.SectionData[sec.Name]; ok {
			sections = append(sections, struct {
				Name string
				Data []byte
			}{sec.Name, data})
		}
	}

	baseHeaderSize := uint32(5 + 4 + 4 + 4 + len(c.Header.TargetTriple))
	var sectionIndexSize uint32 = 4
	for _, sec := range c.Header.Sections {
		sectionIndexSize += 4 + uint32(len(sec.Name)) + 4 + 4
	}
	curOff := baseHeaderSize + sectionIndexSize

	if _, err := writer.WriteString(MAGIC); err != nil {
		return err
	}
	if err := writeU32Be(writer, VERSION_V4); err != nil {
		return err
	}
	if err := writeU32Be(writer, c.Header.Flags); err != nil {
		return err
	}
	if err := writeString(writer, c.Header.TargetTriple); err != nil {
		return err
	}

	if err := writeU32Be(writer, uint32(len(sections))); err != nil {
		return err
	}
	for _, sec := range sections {
		if err := writeString(writer, sec.Name); err != nil {
			return err
		}
		if err := writeU32Be(writer, curOff); err != nil {
			return err
		}
		if err := writeU32Be(writer, uint32(len(sec.Data))); err != nil {
			return err
		}
		curOff += uint32(len(sec.Data))
	}

	for _, sec := range sections {
		if _, err := writer.Write(sec.Data); err != nil {
			return err
		}
	}
	return nil
}

func ParseVxobjV4(data []byte) (*VxObjV4Container, error) {
	if len(data) < 5 {
		return nil, fmt.Errorf("invalid magic")
	}
	if string(data[0:5]) != MAGIC {
		return nil, fmt.Errorf("invalid magic")
	}

	reader := bytes.NewReader(data[5:])

	version, err := readU32Be(reader)
	if err != nil {
		return nil, err
	}
	if version != VERSION_V4 {
		return nil, fmt.Errorf("unsupported version")
	}

	flags, err := readU32Be(reader)
	if err != nil {
		return nil, err
	}

	targetTriple, err := readString(reader)
	if err != nil {
		return nil, err
	}

	numSections, err := readU32Be(reader)
	if err != nil {
		return nil, err
	}

	var sections []VxObjV4SectionIndex
	sectionData := make(map[string][]byte)

	for i := uint32(0); i < numSections; i++ {
		name, err := readString(reader)
		if err != nil {
			return nil, err
		}
		offset, err := readU32Be(reader)
		if err != nil {
			return nil, err
		}
		size, err := readU32Be(reader)
		if err != nil {
			return nil, err
		}

		end := offset + size
		if uint64(end) > uint64(len(data)) {
			return nil, fmt.Errorf("section data truncated")
		}

		sectionData[name] = data[offset:end]
		sections = append(sections, VxObjV4SectionIndex{
			Name:   name,
			Offset: offset,
			Size:   size,
		})
	}

	return &VxObjV4Container{
		Header: VxObjV4Header{
			Version:      version,
			Flags:        flags,
			TargetTriple: targetTriple,
			Sections:     sections,
		},
		SectionData: sectionData,
	}, nil
}

type ExternalDependency struct {
	Name       string
	Path       *string
	IsOptional bool
}

func (d *ExternalDependency) ToBytes() []byte {
	var buf bytes.Buffer
	buf.WriteString(d.Name)
	buf.WriteByte(0)
	if d.Path != nil {
		buf.WriteString(*d.Path)
	}
	buf.WriteByte(0)
	if d.IsOptional {
		buf.WriteByte('1')
	} else {
		buf.WriteByte('0')
	}
	buf.WriteByte(0)
	return buf.Bytes()
}

func ExternalDependencyFromBytes(data []byte) *ExternalDependency {
	parts := bytes.Split(data, []byte{0})
	if len(parts) == 0 {
		return nil
	}

	name := string(parts[0])
	var path *string
	if len(parts) > 1 && len(parts[1]) > 0 {
		p := string(parts[1])
		path = &p
	}
	isOptional := len(parts) > 2 && len(parts[2]) > 0 && parts[2][0] == '1'

	return &ExternalDependency{
		Name:       name,
		Path:       path,
		IsOptional: isOptional,
	}
}

func SerializeExternalDeps(deps []ExternalDependency) []byte {
	var buf bytes.Buffer
	for _, dep := range deps {
		buf.Write(dep.ToBytes())
	}
	return buf.Bytes()
}

func DeserializeExternalDeps(data []byte) []ExternalDependency {
	var deps []ExternalDependency
	pos := 0

	for pos < len(data) {
		nameEnd := bytes.IndexByte(data[pos:], 0)
		if nameEnd == -1 {
			break
		}
		name := string(data[pos : pos+nameEnd])
		pos += nameEnd + 1
		if pos >= len(data) {
			break
		}

		pathEnd := bytes.IndexByte(data[pos:], 0)
		if pathEnd == -1 {
			break
		}
		path := string(data[pos : pos+pathEnd])
		pos += pathEnd + 1
		if pos >= len(data) {
			break
		}

		isOptional := data[pos] == '1'
		pos++

		var pathPtr *string
		if path != "" {
			pathPtr = &path
		}

		deps = append(deps, ExternalDependency{
			Name:       name,
			Path:       pathPtr,
			IsOptional: isOptional,
		})
	}

	return deps
}

func WriteVxobjV4(writer *bytes.Buffer, targetTriple string, typeIrData []byte, debugData []byte, sourceMapData []byte, externalDeps []ExternalDependency) error {
	container := NewVxObjV4Container(targetTriple)

	if len(typeIrData) > 0 {
		container.SetSection(SECTION_TYPE_IR, typeIrData)
	}
	if len(debugData) > 0 {
		container.SetSection(SECTION_DEBUG, debugData)
	}
	if len(sourceMapData) > 0 {
		container.SetSection(SECTION_SOURCE_MAP, sourceMapData)
	}
	if len(externalDeps) > 0 {
		depsData := SerializeExternalDeps(externalDeps)
		container.SetSection(SECTION_EXTERNAL_DEPS, depsData)
		container.SetExternalDepsFlag(true)
	}

	return container.Write(writer)
}

func writeU32Be(writer *bytes.Buffer, v uint32) error {
	var buf [4]byte
	binary.BigEndian.PutUint32(buf[:], v)
	_, err := writer.Write(buf[:])
	return err
}

func writeString(writer *bytes.Buffer, s string) error {
	if err := writeU32Be(writer, uint32(len(s))); err != nil {
		return err
	}
	_, err := writer.WriteString(s)
	return err
}

func readU32Be(reader *bytes.Reader) (uint32, error) {
	var buf [4]byte
	if _, err := reader.Read(buf[:]); err != nil {
		return 0, err
	}
	return binary.BigEndian.Uint32(buf[:]), nil
}

func readString(reader *bytes.Reader) (string, error) {
	len, err := readU32Be(reader)
	if err != nil {
		return "", err
	}
	buf := make([]byte, len)
	if _, err := reader.Read(buf); err != nil {
		return "", err
	}
	return string(buf), nil
}

func DumpSectionStats(data []byte) string {
	container, err := ParseVxobjV4(data)
	if err != nil {
		return fmt.Sprintf("Parse error: %v\n", err)
	}

	var buf bytes.Buffer
	fmt.Fprintf(&buf, "VXOBJ v4 container:\n")
	fmt.Fprintf(&buf, "  Target: %s\n", container.Header.TargetTriple)
	fmt.Fprintf(&buf, "  Sections:\n")
	for _, sec := range container.Header.Sections {
		data := container.GetSection(sec.Name)
		if data == nil {
			data = []byte{}
		}
		fmt.Fprintf(&buf, "    %s: %d bytes\n", sec.Name, len(data))
	}
	return buf.String()
}

type SectionType int

const (
	TypeIRSection SectionType = iota
	DebugSection
	SourceMapSection
	ExternalDepsSection
)

type Section struct {
	Type SectionType
	Name string
	Data []byte
}

type VXOBJFile struct {
	Magic        [5]byte
	Version      uint8
	Flags        uint32
	TargetTriple string
	Sections     []Section
}

// BuildVXOBJ serializes a TypeIR module into a VXOBJ v4 container.
// If codeData is non-empty, a Code section is included for AOT native code.
func BuildVXOBJ(module *typeir.TypeIRModule, targetTriple string, codeData []byte) ([]byte, error) {
	if module == nil {
		return nil, fmt.Errorf("module is nil")
	}

	// Convert TypeIRModule → TypeModule, preserving linkage entries.
	linkage := make(map[uint32]typeir.LinkageEntry)
	if module.Linkage != nil {
		for k, v := range module.Linkage {
			linkage[k] = v
		}
	}
	tm := &typeir.TypeModule{
		Name:        module.Name,
		Functions:   module.Functions,
		Globals:     module.Globals,
		StructTypes: module.StructTypes,
		Linkage:     linkage,
	}

	// Serialize TypeIR to binary format
	typeIRData, err := typeir.SerializeTypeModule(tm)
	if err != nil {
		return nil, err
	}

	container := NewVxObjV4Container(targetTriple)
	container.SetSection(SECTION_TYPE_IR, typeIRData)
	if len(codeData) > 0 {
		container.SetSection(SECTION_CODE, codeData)
	}

	var buf bytes.Buffer
	if err := container.Write(&buf); err != nil {
		return nil, err
	}
	return buf.Bytes(), nil
}
