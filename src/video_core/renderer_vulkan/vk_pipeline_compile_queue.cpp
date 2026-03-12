// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/vk_pipeline_compile_queue.h"

namespace Vulkan {

PipelineCompileQueue::PipelineCompileQueue(u32 worker_count) : worker_count_{worker_count} {}

PipelineCompileQueue::~PipelineCompileQueue() {
    Stop();
}

void PipelineCompileQueue::Start() {
    if (running_.exchange(true)) {
        return;
    }
    workers_.reserve(worker_count_);
    for (u32 i = 0; i < worker_count_; ++i) {
        workers_.emplace_back([this] { WorkerLoop(); });
    }
}

void PipelineCompileQueue::Stop() {
    if (!running_.exchange(false)) {
        return;
    }
    cv_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) {
            t.join();
        }
    }
    workers_.clear();
    queue_depth_.store(0, std::memory_order_relaxed);
}

PipelineCompileQueue::EnqueueResult PipelineCompileQueue::TryEnqueue(Task task,
                                                                     u32 max_queue_depth) {
    if (!running_) {
        return {};
    }

    EnqueueResult result{};
    {
        std::scoped_lock lk{mutex_};
        result.queue_depth = static_cast<u32>(tasks_.size());
        if (result.queue_depth >= max_queue_depth) {
            queue_depth_.store(result.queue_depth, std::memory_order_relaxed);
            return result;
        }

        tasks_.push(std::move(task));
        result.queue_depth = static_cast<u32>(tasks_.size());
        result.enqueued = true;
        queue_depth_.store(result.queue_depth, std::memory_order_relaxed);
    }
    cv_.notify_one();
    return result;
}

u32 PipelineCompileQueue::QueueDepth() const {
    return queue_depth_.load(std::memory_order_relaxed);
}

u64 PipelineCompileQueue::CompletedTasks() const {
    return completed_.load(std::memory_order_relaxed);
}

void PipelineCompileQueue::WorkerLoop() {
    while (true) {
        Task task;
        {
            std::unique_lock lk{mutex_};
            cv_.wait(lk, [this] { return !running_ || !tasks_.empty(); });
            if (!running_ && tasks_.empty()) {
                return;
            }
            task = std::move(tasks_.front());
            tasks_.pop();
            queue_depth_.store(static_cast<u32>(tasks_.size()), std::memory_order_relaxed);
        }

        if (task) {
            task();
            completed_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

} // namespace Vulkan
