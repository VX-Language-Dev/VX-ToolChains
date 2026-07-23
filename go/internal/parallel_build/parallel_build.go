package parallelbuild

import (
	"sync"
	"time"

	"github.com/vx-lang/vxc/internal/logger"
)

// TaskStatus 任务状态
type TaskStatus int

const (
	// Pending 待执行
	Pending TaskStatus = iota
	// Running 执行中
	Running
	// Success 成功
	Success
	// Failed 失败
	Failed
)

// BuildTask 构建任务
type BuildTask struct {
	ID         int
	Source     string
	Output     string
	OptLevel   uint8
	Status     TaskStatus
	ErrorMsg   string
	DurationUs int64
}

// NewBuildTask 创建新的构建任务
func NewBuildTask(id int, source string, output string, optLevel uint8) BuildTask {
	return BuildTask{
		ID:       id,
		Source:   source,
		Output:   output,
		OptLevel: optLevel,
		Status:   Pending,
	}
}

// BuildResult 构建结果
type BuildResult struct {
	Total           int
	Succeeded       int
	Failed          int
	Skipped         int
	TotalDurationUs int64
}

// Summary 返回构建结果摘要
func (br *BuildResult) Summary() string {
	if br.Failed > 0 {
		return "FAILED"
	}
	if br.Succeeded == 0 {
		return "SKIPPED"
	}
	return "OK"
}

// CompileFn 编译函数类型
type CompileFn func(task *BuildTask) error

// BuildScheduler 构建调度器
type BuildScheduler struct {
	Tasks       []BuildTask
	MaxParallel int
	Logger      *logger.Logger
}

// NewBuildScheduler 创建新的构建调度器
func NewBuildScheduler(maxParallel int) *BuildScheduler {
	return &BuildScheduler{
		MaxParallel: maxParallel,
	}
}

// SetLogger 设置日志记录器
func (bs *BuildScheduler) SetLogger(log *logger.Logger) {
	bs.Logger = log
}

// AddTask 添加构建任务
func (bs *BuildScheduler) AddTask(source string, output string, optLevel uint8) int {
	id := len(bs.Tasks)
	bs.Tasks = append(bs.Tasks, NewBuildTask(id, source, output, optLevel))
	return id
}

// nowMicros 获取当前时间的微秒数
func nowMicros() int64 {
	return time.Now().UnixMicro()
}

// Execute 并行执行构建任务
func (bs *BuildScheduler) Execute(compileFn CompileFn) BuildResult {
	result := BuildResult{
		Total: len(bs.Tasks),
	}

	if len(bs.Tasks) == 0 {
		return result
	}

	if bs.Logger != nil {
		bs.Logger.Info("开始并行构建: %d 个任务, 最大并行度: %d", len(bs.Tasks), bs.MaxParallel)
	}

	var mu sync.Mutex
	var wg sync.WaitGroup

	// 使用信号量通道控制并发度
	sem := make(chan struct{}, bs.MaxParallel)

	for i := range bs.Tasks {
		sem <- struct{}{} // 获取信号量
		wg.Add(1)

		go func(idx int) {
			defer wg.Done()
			defer func() { <-sem }() // 释放信号量

			task := &bs.Tasks[idx]
			mu.Lock()
			task.Status = Running
			mu.Unlock()

			start := nowMicros()
			if err := compileFn(task); err != nil {
				mu.Lock()
				task.Status = Failed
				task.ErrorMsg = err.Error()
				mu.Unlock()
				if bs.Logger != nil {
					bs.Logger.Err("任务 %d 失败: %s -> %s (%s)", task.ID, task.Source, task.Output, err.Error())
				}
				return
			}

			mu.Lock()
			task.Status = Success
			task.DurationUs = nowMicros() - start
			result.TotalDurationUs += task.DurationUs
			mu.Unlock()

			if bs.Logger != nil {
				bs.Logger.Debug("任务 %d 完成: %s (%dus)", task.ID, task.Source, task.DurationUs)
			}
		}(i)
	}

	wg.Wait()

	// 统计结果
	for _, task := range bs.Tasks {
		switch task.Status {
		case Success:
			result.Succeeded++
		case Failed:
			result.Failed++
		case Pending:
			result.Skipped++
		}
	}

	if bs.Logger != nil {
		bs.Logger.Info("构建完成: %d 成功, %d 失败, %d 跳过 (总耗时 %dus)",
			result.Succeeded, result.Failed, result.Skipped, result.TotalDurationUs)
	}

	return result
}

// ExecuteSequential 顺序执行构建任务
func (bs *BuildScheduler) ExecuteSequential(compileFn CompileFn) BuildResult {
	result := BuildResult{
		Total: len(bs.Tasks),
	}

	for i := range bs.Tasks {
		task := &bs.Tasks[i]
		task.Status = Running
		start := nowMicros()

		if err := compileFn(task); err != nil {
			task.Status = Failed
			task.ErrorMsg = err.Error()
			result.Failed++
			if bs.Logger != nil {
				bs.Logger.Err("任务 %d 失败: %s (%s)", task.ID, task.Source, err.Error())
			}
			continue
		}

		task.Status = Success
		task.DurationUs = nowMicros() - start
		result.Succeeded++
		result.TotalDurationUs += task.DurationUs
	}

	return result
}
