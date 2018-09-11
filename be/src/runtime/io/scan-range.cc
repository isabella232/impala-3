// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "runtime/exec-env.h"
#include "runtime/io/disk-io-mgr.h"
#include "runtime/io/disk-io-mgr-internal.h"
#include "runtime/io/hdfs-file-reader.h"
#include "runtime/io/local-file-reader.h"
#include "util/error-util.h"
#include "util/hdfs-util.h"

#include "common/names.h"

using namespace impala;
using namespace impala::io;

// TODO: Run perf tests and empirically settle on the most optimal default value for the
// read buffer size. Currently setting it as 128k for the same reason as for S3, i.e.
// due to JNI array allocation and memcpy overhead, 128k was emperically found to have the
// least overhead.
DEFINE_int64(adls_read_chunk_size, 128 * 1024, "The maximum read chunk size to use when "
    "reading from ADLS.");

// Implementation of the ScanRange functionality. Each ScanRange contains a queue
// of ready buffers. For each ScanRange, there is only a single producer and
// consumer thread, i.e. only one disk thread will push to a scan range at
// any time and only one thread will remove from the queue. This is to guarantee
// that buffers are queued and read in file order.
bool ScanRange::EnqueueReadyBuffer(unique_ptr<BufferDescriptor> buffer) {
  DCHECK(buffer->buffer_ != nullptr) << "Cannot enqueue freed buffer";
  {
    unique_lock<mutex> scan_range_lock(lock_);
    DCHECK(Validate()) << DebugString();
    DCHECK(!eosr_queued_);
    if (!buffer->is_cached()) {
      // All non-cached buffers are enqueued by disk threads. Indicate that the read
      // finished.
      DCHECK(read_in_flight_);
      read_in_flight_ = false;
    }
    if (!cancel_status_.ok()) {
      // This range has been cancelled, no need to enqueue the buffer.
      CleanUpBuffer(scan_range_lock, move(buffer));
      // One or more threads may be blocked in WaitForInFlightRead() waiting for the read
      // to complete. Wake up all of them.
      buffer_ready_cv_.NotifyAll();
      return false;
    }
    // Clean up any surplus buffers. E.g. we may have allocated too many if the file was
    // shorter than expected.
    if (buffer->eosr()) CleanUpUnusedBuffers(scan_range_lock);
    eosr_queued_ = buffer->eosr();
    ready_buffers_.emplace_back(move(buffer));
  }
  buffer_ready_cv_.NotifyOne();
  return true;
}

Status ScanRange::GetNext(unique_ptr<BufferDescriptor>* buffer) {
  DCHECK(*buffer == nullptr);
  bool eosr;
  {
    unique_lock<mutex> scan_range_lock(lock_);
    DCHECK(Validate()) << DebugString();
    while (!all_buffers_returned(scan_range_lock) && ready_buffers_.empty()) {
      buffer_ready_cv_.Wait(scan_range_lock);
    }
    // No more buffers to return - return the cancel status or OK if not cancelled.
    if (all_buffers_returned(scan_range_lock)) {
      // Wait until read finishes to ensure buffers are freed.
      while (read_in_flight_) buffer_ready_cv_.Wait(scan_range_lock);
      DCHECK_EQ(0, ready_buffers_.size());
      return cancel_status_;
    }

    // Remove the first ready buffer from the queue and return it
    DCHECK(!ready_buffers_.empty());
    *buffer = move(ready_buffers_.front());
    ready_buffers_.pop_front();
    eosr = (*buffer)->eosr();
    DCHECK(!eosr || unused_iomgr_buffers_.empty()) << DebugString();
  }

  // Update tracking counters. The buffer has now moved from the IoMgr to the caller.
  if (eosr) reader_->RemoveActiveScanRange(this);
  num_buffers_in_reader_.Add(1);
  return Status::OK();
}

void ScanRange::ReturnBuffer(unique_ptr<BufferDescriptor> buffer_desc) {
  vector<unique_ptr<BufferDescriptor>> buffers;
  buffers.emplace_back(move(buffer_desc));
  AddUnusedBuffers(move(buffers), true);
}

void ScanRange::AddUnusedBuffers(vector<unique_ptr<BufferDescriptor>>&& buffers,
      bool returned) {
  DCHECK_GT(buffers.size(), 0);
  /// Keep track of whether the range was unblocked in this function. If so, we need
  /// to schedule it so it resumes progress.
  bool unblocked = false;
  {
    unique_lock<mutex> scan_range_lock(lock_);
    if (returned) {
      // Buffers were in reader but now aren't.
      num_buffers_in_reader_.Add(-buffers.size());
    }

    for (unique_ptr<BufferDescriptor>& buffer : buffers) {
      // We should not hold onto the buffers in the following cases:
      // 1. the scan range is using external buffers, e.g. cached buffers.
      // 2. the scan range is cancelled
      // 3. the scan range already hit eosr
      // 4. we already have enough buffers to read the remainder of the scan range.
      if (external_buffer_tag_ != ExternalBufferTag::NO_BUFFER
          || !cancel_status_.ok()
          || eosr_queued_
          || unused_iomgr_buffer_bytes_ >= len_ - iomgr_buffer_cumulative_bytes_used_) {
        CleanUpBuffer(scan_range_lock, move(buffer));
      } else {
        unused_iomgr_buffer_bytes_ += buffer->buffer_len();
        unused_iomgr_buffers_.emplace_back(move(buffer));
        if (blocked_on_buffer_) {
          blocked_on_buffer_ = false;
          unblocked = true;
        }
      }
    }
  }
  // Must drop the ScanRange lock before acquiring the RequestContext lock.
  if (unblocked) {
    unique_lock<mutex> reader_lock(reader_->lock_);
    // Reader may have been cancelled after we dropped 'scan_range_lock' above.
    if (reader_->state_ == RequestContext::Cancelled) {
      DCHECK(!cancel_status_.ok());
    } else {
      reader_->ScheduleScanRange(reader_lock, this);
    }
  }
}

unique_ptr<BufferDescriptor> ScanRange::GetUnusedBuffer(
    const unique_lock<mutex>& scan_range_lock) {
  DCHECK(scan_range_lock.mutex() == &lock_ && scan_range_lock.owns_lock());
  if (unused_iomgr_buffers_.empty()) return nullptr;
  unique_ptr<BufferDescriptor> result = move(unused_iomgr_buffers_.back());
  unused_iomgr_buffers_.pop_back();
  unused_iomgr_buffer_bytes_ -= result->buffer_len();
  return result;
}

int ScanRange::BytesRead() const {
  DCHECK(file_reader_ != nullptr);
  return file_reader_->bytes_read();
}

ReadOutcome ScanRange::DoRead(int disk_id) {
  int64_t bytes_remaining = len_ - BytesRead();
  DCHECK_GT(bytes_remaining, 0);

  unique_ptr<BufferDescriptor> buffer_desc;
  {
    unique_lock<mutex> lock(lock_);
    DCHECK(!read_in_flight_);
    if (!cancel_status_.ok()) return ReadOutcome::CANCELLED;

    if (external_buffer_tag_ == ScanRange::ExternalBufferTag::CLIENT_BUFFER) {
      buffer_desc = unique_ptr<BufferDescriptor>(new BufferDescriptor(
          this, client_buffer_.data, client_buffer_.len));
    } else {
      DCHECK(external_buffer_tag_ == ScanRange::ExternalBufferTag::NO_BUFFER)
          << "This code path does not handle other buffer types, i.e. HDFS cache"
          << static_cast<int>(external_buffer_tag_);
      buffer_desc = GetUnusedBuffer(lock);
      if (buffer_desc == nullptr) {
        // No buffer available - the range will be rescheduled when a buffer is added.
        blocked_on_buffer_ = true;
        return ReadOutcome::BLOCKED_ON_BUFFER;
      }
      iomgr_buffer_cumulative_bytes_used_ += buffer_desc->buffer_len();
    }
    read_in_flight_ = true;
  }

  // No locks in this section.  Only working on local vars.  We don't want to hold a
  // lock across the read call.
  Status read_status = file_reader_->Open(is_file_handle_caching_enabled());
  if (read_status.ok()) {
    COUNTER_ADD_IF_NOT_NULL(reader_->active_read_thread_counter_, 1L);
    COUNTER_BITOR_IF_NOT_NULL(reader_->disks_accessed_bitmap_, 1LL << disk_id);

    read_status = file_reader_->ReadFromPos(offset_ + BytesRead(), buffer_desc->buffer_,
        min(len() - BytesRead(), buffer_desc->buffer_len_), &buffer_desc->len_,
        &buffer_desc->eosr_);
    buffer_desc->scan_range_offset_ = BytesRead() - buffer_desc->len_;

    COUNTER_ADD_IF_NOT_NULL(reader_->bytes_read_counter_, buffer_desc->len_);
    COUNTER_ADD_IF_NOT_NULL(reader_->active_read_thread_counter_, -1L);
  }

  DCHECK(buffer_desc->buffer_ != nullptr);
  DCHECK(!buffer_desc->is_cached()) << "HDFS cache reads don't go through this code path.";
  if (!read_status.ok()) {
    // Free buffer to release resources before we cancel the range so that all buffers
    // are freed at cancellation.
    buffer_desc->Free();
    buffer_desc.reset();

    // Propagate 'read_status' to the scan range. This will also wake up any waiting
    // threads.
    CancelInternal(read_status, true);
    // No more reads for this scan range - we can close it.
    file_reader_->Close();
    return ReadOutcome::CANCELLED;
  }

  // After calling EnqueueReadyBuffer(), it is no longer valid to touch 'buffer_desc'.
  // Store the state we need before calling EnqueueReadyBuffer().
  bool eosr = buffer_desc->eosr_;
  // Read successful - enqueue the buffer and return the appropriate outcome.
  if (!EnqueueReadyBuffer(move(buffer_desc))) return ReadOutcome::CANCELLED;
  if (eosr) {
    // No more reads for this scan range - we can close it.
    file_reader_->Close();
    return ReadOutcome::SUCCESS_EOSR;
  }
  return ReadOutcome::SUCCESS_NO_EOSR;
}

void ScanRange::SetBlockedOnBuffer() {
  unique_lock<mutex> lock(lock_);
  blocked_on_buffer_ = true;
}

void ScanRange::CleanUpBuffer(
    const boost::unique_lock<boost::mutex>& scan_range_lock,
    unique_ptr<BufferDescriptor> buffer_desc) {
  DCHECK(scan_range_lock.mutex() == &lock_ && scan_range_lock.owns_lock());
  DCHECK(buffer_desc != nullptr);
  DCHECK_EQ(this, buffer_desc->scan_range_);
  buffer_desc->Free();

  if (all_buffers_returned(scan_range_lock) && num_buffers_in_reader_.Load() == 0) {
    // Close the scan range if there are no more buffers in the reader and no more buffers
    // will be returned to readers in future. Close() is idempotent so it is ok to call
    // multiple times during cleanup so long as the range is actually finished.
    file_reader_->Close();
  }
}

void ScanRange::CleanUpBuffers(vector<unique_ptr<BufferDescriptor>>&& buffers) {
  unique_lock<mutex> lock(lock_);
  for (unique_ptr<BufferDescriptor>& buffer : buffers) CleanUpBuffer(lock, move(buffer));
}

void ScanRange::CleanUpUnusedBuffers(const unique_lock<mutex>& scan_range_lock) {
  while (!unused_iomgr_buffers_.empty()) {
    CleanUpBuffer(scan_range_lock, GetUnusedBuffer(scan_range_lock));
  }
}

void ScanRange::Cancel(const Status& status) {
  // Cancelling a range that was never started, ignore.
  if (io_mgr_ == nullptr) return;
  CancelInternal(status, false);
  // Wait until an in-flight read is finished. The read thread will clean up the
  // buffer it used. Once the range is cancelled, no more reads should be started.
  WaitForInFlightRead();
  reader_->RemoveActiveScanRange(this);
}

void ScanRange::CancelInternal(const Status& status, bool read_error) {
  DCHECK(io_mgr_ != nullptr);
  DCHECK(!status.ok());
  {
    // Grab both locks to make sure that we don't change 'cancel_status_' while other
    // threads are in critical sections.
    unique_lock<mutex> scan_range_lock(lock_);
    {
      unique_lock<SpinLock> fs_lock(file_reader_->lock());
      DCHECK(Validate()) << DebugString();
      // If already cancelled, preserve the original reason for cancellation. Most of the
      // cleanup is not required if already cancelled, but we need to set
      // 'read_in_flight_' to false.
      if (cancel_status_.ok()) cancel_status_ = status;
    }

    /// Clean up 'ready_buffers_' while still holding 'lock_' to prevent other threads
    /// from seeing inconsistent state.
    while (!ready_buffers_.empty()) {
      CleanUpBuffer(scan_range_lock, move(ready_buffers_.front()));
      ready_buffers_.pop_front();
    }

    /// Clean up buffers that we don't need any more because we won't read any more data.
    CleanUpUnusedBuffers(scan_range_lock);
    if (read_error) {
      DCHECK(read_in_flight_);
      read_in_flight_ = false;
    }
  }
  buffer_ready_cv_.NotifyAll();

  // For cached buffers, we can't close the range until the cached buffer is returned.
  // Close() is called from ScanRange::CleanUpBufferLocked().
  if (external_buffer_tag_ != ExternalBufferTag::CACHED_BUFFER) file_reader_->Close();
}

void ScanRange::WaitForInFlightRead() {
  unique_lock<mutex> scan_range_lock(lock_);
  while (read_in_flight_) buffer_ready_cv_.Wait(scan_range_lock);
}

string ScanRange::DebugString() const {
  stringstream ss;
  ss << "file=" << file_ << " disk_id=" << disk_id_ << " offset=" << offset_;
  if (file_reader_) ss << " " << file_reader_->DebugString();
  ss << " cancel_status=" << cancel_status_.GetDetail()
     << " buffer_queue=" << ready_buffers_.size()
     << " num_buffers_in_readers=" << num_buffers_in_reader_.Load()
     << " unused_iomgr_buffers=" << unused_iomgr_buffers_.size()
     << " unused_iomgr_buffer_bytes=" << unused_iomgr_buffer_bytes_
     << " blocked_on_buffer=" << blocked_on_buffer_;
  return ss.str();
}

bool ScanRange::Validate() {
  if (BytesRead() > len_) {
    LOG(ERROR) << "Bytes read tracking is wrong. Shouldn't read past the scan range."
                 << " bytes_read_=" << BytesRead() << " len_=" << len_;
    return false;
  }
  if (!cancel_status_.ok() && !ready_buffers_.empty()) {
    LOG(ERROR) << "Cancelled range should not have queued buffers " << DebugString();
    return false;
  }
  int64_t unused_iomgr_buffer_bytes = 0;
  for (auto& buffer : unused_iomgr_buffers_)
    unused_iomgr_buffer_bytes += buffer->buffer_len();
  if (unused_iomgr_buffer_bytes != unused_iomgr_buffer_bytes_) {
    LOG(ERROR) << "unused_iomgr_buffer_bytes_ incorrect actual: "
               << unused_iomgr_buffer_bytes_
               << " vs. expected: " << unused_iomgr_buffer_bytes;
    return false;
  }
  bool is_finished = !cancel_status_.ok() || eosr_queued_;
  if (is_finished && !unused_iomgr_buffers_.empty()) {
    LOG(ERROR) << "Held onto too many buffers " << unused_iomgr_buffers_.size()
               << " bytes: " << unused_iomgr_buffer_bytes_
               << " cancel_status: " << cancel_status_.GetDetail()
               << " eosr_queued: " << eosr_queued_;
    return false;
  }
  if (!is_finished && blocked_on_buffer_ && !unused_iomgr_buffers_.empty()) {
    LOG(ERROR) << "Blocked despite having buffers: " << DebugString();
    return false;
  }
  return true;
}

ScanRange::ScanRange()
  : RequestRange(RequestType::READ),
    external_buffer_tag_(ExternalBufferTag::NO_BUFFER) {}

ScanRange::~ScanRange() {
  DCHECK(external_buffer_tag_ != ExternalBufferTag::CACHED_BUFFER)
      << "Cached buffer was not released.";
  DCHECK(!read_in_flight_);
  DCHECK_EQ(0, ready_buffers_.size());
  DCHECK_EQ(0, num_buffers_in_reader_.Load());
}

void ScanRange::Reset(hdfsFS fs, const char* file, int64_t len, int64_t offset,
    int disk_id, bool expected_local, const BufferOpts& buffer_opts, void* meta_data) {
  DCHECK(ready_buffers_.empty());
  DCHECK(!read_in_flight_);
  DCHECK(file != nullptr);
  DCHECK_GE(len, 0);
  DCHECK_GE(offset, 0);
  DCHECK(buffer_opts.client_buffer_ == nullptr ||
         buffer_opts.client_buffer_len_ >= len_);
  fs_ = fs;
  if (fs_) {
    file_reader_ = make_unique<HdfsFileReader>(this, fs_, expected_local);
  } else {
    file_reader_ = make_unique<LocalFileReader>(this);
  }
  file_ = file;
  len_ = len;
  offset_ = offset;
  disk_id_ = disk_id;
  try_cache_ = buffer_opts.try_cache_;
  mtime_ = buffer_opts.mtime_;
  meta_data_ = meta_data;
  if (buffer_opts.client_buffer_ != nullptr) {
    external_buffer_tag_ = ExternalBufferTag::CLIENT_BUFFER;
    client_buffer_.data = buffer_opts.client_buffer_;
    client_buffer_.len = buffer_opts.client_buffer_len_;
  } else {
    external_buffer_tag_ = ExternalBufferTag::NO_BUFFER;
  }
  expected_local_ = expected_local;
  io_mgr_ = nullptr;
  reader_ = nullptr;
}

void ScanRange::InitInternal(DiskIoMgr* io_mgr, RequestContext* reader) {
  DCHECK(!read_in_flight_);
  io_mgr_ = io_mgr;
  reader_ = reader;
  unused_iomgr_buffer_bytes_ = 0;
  iomgr_buffer_cumulative_bytes_used_ = 0;
  cancel_status_ = Status::OK();
  eosr_queued_ = false;
  blocked_on_buffer_ = false;
  file_reader_->ResetState();
  DCHECK(Validate()) << DebugString();
}

int64_t ScanRange::MaxReadChunkSize() const {
  // S3 InputStreams don't support DIRECT_READ (i.e. java.nio.ByteBuffer read()
  // interface).  So, hdfsRead() needs to allocate a Java byte[] and copy the data out.
  // Profiles show that both the JNI array allocation and the memcpy adds much more
  // overhead for larger buffers, so limit the size of each read request.  128K was
  // chosen empirically by trying values between 4K and 8M and optimizing for lower CPU
  // utilization and higher S3 througput.
  if (disk_id_ == io_mgr_->RemoteS3DiskId()) {
    DCHECK(IsS3APath(file()));
    return 128 * 1024;
  }
  if (disk_id_ == io_mgr_->RemoteAdlsDiskId()) {
    DCHECK(IsADLSPath(file()));
    return FLAGS_adls_read_chunk_size;
  }
  // The length argument of hdfsRead() is an int. Ensure we don't overflow it.
  return numeric_limits<int>::max();
}

Status ScanRange::ReadFromCache(
    const unique_lock<mutex>& reader_lock, bool* read_succeeded) {
  DCHECK(reader_lock.mutex() == &reader_->lock_ && reader_lock.owns_lock());
  DCHECK(try_cache_);
  DCHECK_EQ(BytesRead(), 0);
  *read_succeeded = false;
  Status status = file_reader_->Open(false);
  if (!status.ok()) return status;

  // Cached reads not supported on local filesystem.
  if (fs_ == nullptr) return Status::OK();

  // Check cancel status.
  {
    unique_lock<mutex> lock(lock_);
    RETURN_IF_ERROR(cancel_status_);
  }

  DCHECK(external_buffer_tag_ == ExternalBufferTag::NO_BUFFER);
  void* buffer = file_reader_->CachedFile();
  if (buffer != nullptr) external_buffer_tag_ = ExternalBufferTag::CACHED_BUFFER;
  // Data was not cached, caller will fall back to normal read path.
  if (external_buffer_tag_ != ExternalBufferTag::CACHED_BUFFER) {
    VLOG_QUERY << "Cache read failed for scan range: " << DebugString()
               << ". Switching to disk read path.";
    // Clean up the scan range state before re-issuing it.
    file_reader_->Close();
    return Status::OK();
  }
  int bytes_read = BytesRead();
  // A partial read can happen when files are truncated.
  // TODO: If HDFS ever supports partially cached blocks, we'll have to distinguish
  // between errors and partially cached blocks here.
  if (bytes_read < len()) {
    VLOG_QUERY << "Error reading file from HDFS cache: " << file_ << ". Expected "
      << len() << " bytes, but read " << bytes_read << ". Switching to disk read path.";
    // Close the scan range. 'read_succeeded' is still false, so the caller will fall back
    // to non-cached read of this scan range.
    file_reader_->Close();
    return Status::OK();
  }

  // Create a single buffer desc for the entire scan range and enqueue that.
  // The memory is owned by the HDFS java client, not the Impala backend.
  unique_ptr<BufferDescriptor> desc = unique_ptr<BufferDescriptor>(new BufferDescriptor(
      this, reinterpret_cast<uint8_t*>(buffer), 0));
  desc->len_ = bytes_read;
  desc->scan_range_offset_ = 0;
  desc->eosr_ = true;
  EnqueueReadyBuffer(move(desc));
  COUNTER_ADD_IF_NOT_NULL(reader_->bytes_read_counter_, bytes_read);
  *read_succeeded = true;
  return Status::OK();
}

BufferDescriptor::BufferDescriptor(ScanRange* scan_range,
    uint8_t* buffer, int64_t buffer_len)
  : scan_range_(scan_range),
    buffer_(buffer),
    buffer_len_(buffer_len) {
  DCHECK(scan_range != nullptr);
  DCHECK(buffer != nullptr);
  DCHECK_GE(buffer_len, 0);
}

BufferDescriptor::BufferDescriptor(ScanRange* scan_range,
    BufferPool::ClientHandle* bp_client, BufferPool::BufferHandle handle) :
  scan_range_(scan_range),
  buffer_(handle.data()),
  buffer_len_(handle.len()),
  bp_client_(bp_client),
  handle_(move(handle)) {
  DCHECK(scan_range != nullptr);
  DCHECK(bp_client_->is_registered());
  DCHECK(handle_.is_open());
}

void BufferDescriptor::Free() {
  DCHECK(buffer_ != nullptr);
  if (!is_cached() && !is_client_buffer()) {
    // Only buffers that were allocated by DiskIoMgr need to be freed.
    ExecEnv::GetInstance()->buffer_pool()->FreeBuffer(
        bp_client_, &handle_);
  }
  buffer_ = nullptr;
}
