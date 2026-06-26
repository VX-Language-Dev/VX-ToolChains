#!/bin/bash
# VX compiler end-to-end test script
# Tests the native compilation system

# Do not set set -e; handle errors manually

echo "========================================="
echo "VX Native Compilation End-to-End Test"
echo "========================================="
echo

# Color definitions
GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m' # No Color

# Test counters
TESTS_PASSED=0
TESTS_FAILED=0

# Test function
run_test() {
    local test_name="$1"
    local test_cmd="$2"
    local expected="$3"
    
    echo -n "Test: $test_name ... "
    
    if eval "$test_cmd" > /tmp/test_output.txt 2>&1; then
        if [ -n "$expected" ]; then
            if grep -q "$expected" /tmp/test_output.txt; then
                echo -e "${GREEN}PASS${NC}"
                ((TESTS_PASSED++))
            else
                echo -e "${RED}FAIL${NC}"
                echo "  Expected: $expected"
                echo "  Actual output:"
                cat /tmp/test_output.txt | sed 's/^/    /'
                ((TESTS_FAILED++))
            fi
        else
            echo -e "${GREEN}PASS${NC}"
            ((TESTS_PASSED++))
        fi
    else
        echo -e "${RED}FAIL${NC}"
        echo "  Command failed:"
        cat /tmp/test_output.txt | sed 's/^/    /'
        ((TESTS_FAILED++))
    fi
}

# Test executable exit code
test_exit_code() {
    local test_name="$1"
    local executable="$2"
    local expected_code="$3"
    
    echo -n "Test: $test_name ... "
    
    if [ ! -x "$executable" ]; then
        echo -e "${RED}FAIL${NC}"
        echo "  Executable not found or not executable: $executable"
        ((TESTS_FAILED++))
        return
    fi
    
    if "$executable" > /dev/null 2>&1; then
        actual_code=$?
    else
        actual_code=$?
    fi
    
    if [ "$actual_code" -eq "$expected_code" ]; then
        echo -e "${GREEN}PASS${NC} (exit code: $actual_code)"
        ((TESTS_PASSED++))
    else
        echo -e "${RED}FAIL${NC}"
        echo "  Expected exit code: $expected_code"
        echo "  Actual exit code: $actual_code"
        ((TESTS_FAILED++))
    fi
}

# Cleanup function
cleanup() {
    echo
    echo "Cleaning up test files..."
    rm -f test_*.vx test_*.vxco test_static test_dynamic /tmp/test_output.txt
    echo "Cleanup done"
}

# Set trap to ensure cleanup on exit
trap cleanup EXIT

echo "========================================="
echo "Part 1: Static Linking Test"
echo "========================================="
echo

# Create static link test file
cat > test_static.vx << 'EOF'
func add(a: int, b: int):
    return a + b

func main():
    return add(10, 20)
EOF

run_test "Compile static link program" \
    "./target/debug/vxcompiler test_static.vx -o test_static.vxco" \
    "Compiled"

run_test "Link static link program" \
    "./target/debug/vxlinker test_static.vxco --mode native -o test_static" \
    "static=true"

test_exit_code "Static linked program execution" "./test_static" 30

echo
echo "========================================="
echo "Part 2: Dynamic Linking Test"
echo "========================================="
echo

# Create dynamic link test file
cat > test_dynamic.vx << 'EOF'
import libc

func main():
    return 42
EOF

run_test "Compile dynamic link program" \
    "./target/debug/vxcompiler test_dynamic.vx -o test_dynamic.vxco" \
    "Compiled"

run_test "Link dynamic link program" \
    "./target/debug/vxlinker test_dynamic.vxco --mode native -o test_dynamic" \
    "static=false"

test_exit_code "Dynamic linked program execution" "./test_dynamic" 42

echo
echo "========================================="
echo "Part 3: External Dependency Tracking Test"
echo "========================================="
echo

# Test external dependency detection
run_test "Detect external dependencies" \
    "./target/debug/vxlinker test_dynamic.vxco --mode native -o test_dynamic" \
    "External dependencies: \[\"libc\"\]"

# Test no external dependency detection
run_test "Detect no external dependencies" \
    "./target/debug/vxlinker test_static.vxco --mode native -o test_static" \
    "External dependencies: none"

echo
echo "========================================="
echo "Part 4: CLI Output Test"
echo "========================================="
echo

run_test "Compiler English output" \
    "./target/debug/vxcompiler test_static.vx -o test_static.vxco" \
    "Compiled"

run_test "Linker English output" \
    "./target/debug/vxlinker test_static.vxco --mode native -o test_static" \
    "Native linked"

echo
echo "========================================="
echo "Test Summary"
echo "========================================="
echo

echo -e "Passed: ${GREEN}$TESTS_PASSED${NC}"
echo -e "Failed: ${RED}$TESTS_FAILED${NC}"
echo

if [ $TESTS_FAILED -eq 0 ]; then
    echo -e "${GREEN}All tests passed!${NC}"
    exit 0
else
    echo -e "${RED}Some tests failed${NC}"
    exit 1
fi
