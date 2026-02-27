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
}

bool PipelineCompileQueue::Enqueue(Task task) {
    if (!running_) {
        return false;
    }
    {
        std::scoped_lock lk{mutex_};
        tasks_.push(std::move(task));
    }
    cv_.notify_one();
    return true;
}

u32 PipelineCompileQueue::QueueDepth() const {
    std::scoped_lock lk{mutex_};
    return static_cast<u32>(tasks_.size());
}

u64 PipelineCompileQueue::CompletedTasks() const {
    return completed_.load();
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
        }

        if (task) {
            task();
            ++completed_;
        }
    }
}

} // namespace Vulkan
