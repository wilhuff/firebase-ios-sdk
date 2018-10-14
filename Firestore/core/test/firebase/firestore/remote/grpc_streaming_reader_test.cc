/*
 * Copyright 2018 Google
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <initializer_list>
#include <map>
#include <memory>
#include <queue>
#include <utility>
#include <vector>

#include "Firestore/core/src/firebase/firestore/util/async_queue.h"
#include "Firestore/core/src/firebase/firestore/util/executor_std.h"
#include "Firestore/core/test/firebase/firestore/util/create_noop_connectivity_monitor.h"
#include "Firestore/core/test/firebase/firestore/util/grpc_stream_tester.h"
#include "absl/types/optional.h"
#include "grpcpp/support/byte_buffer.h"
#include "gtest/gtest.h"

namespace firebase {
namespace firestore {
namespace remote {

using util::AsyncQueue;
using util::ByteBufferToString;
using util::CompletionEndState;
using util::CreateNoOpConnectivityMonitor;
using util::GetFirestoreErrorCodeName;
using util::GetGrpcErrorCodeName;
using util::GrpcStreamTester;
using util::MakeByteBuffer;
using util::Status;
using util::StatusOr;
using util::StringFormat;
using util::CompletionResult::Error;
using util::CompletionResult::Ok;
using util::internal::ExecutorStd;
using Type = GrpcCompletion::Type;

class GrpcStreamingReaderTest : public testing::Test {
 public:
  GrpcStreamingReaderTest()
      : worker_queue{absl::make_unique<ExecutorStd>()},
        connectivity_monitor{CreateNoOpConnectivityMonitor()},
        tester{&worker_queue, connectivity_monitor.get()},
        reader{tester.CreateStreamingReader()} {
  }

  ~GrpcStreamingReaderTest() {
    if (reader) {
      // It's okay to call `FinishImmediately` more than once.
      KeepPollingGrpcQueue();
      worker_queue.EnqueueBlocking([&] { reader->FinishImmediately(); });
    }
    tester.Shutdown();
  }

  void ForceFinish(std::initializer_list<CompletionEndState> results) {
    tester.ForceFinish(reader->context(), results);
  }
  void ForceFinish(const GrpcStreamTester::CompletionCallback& callback) {
    tester.ForceFinish(reader->context(), callback);
  }
  void ForceFinishAnyTypeOrder(std::initializer_list<CompletionEndState> results) {
    tester.ForceFinishAnyTypeOrder(reader->context(), results);
  }

  void KeepPollingGrpcQueue() {
    tester.KeepPollingGrpcQueue();
  }

  void StartReader() {
    worker_queue.EnqueueBlocking([&] {
      reader->Start(
          [this](const StatusOr<std::vector<grpc::ByteBuffer>>& result) {
            status = result.status();
            if (status->ok()) {
              responses = result.ValueOrDie();
            }
          });
    });
  }

  AsyncQueue worker_queue;
  std::unique_ptr<ConnectivityMonitor> connectivity_monitor;
  GrpcStreamTester tester;

  std::unique_ptr<GrpcStreamingReader> reader;

  absl::optional<Status> status;
  std::vector<grpc::ByteBuffer> responses;
};

// API usage

TEST_F(GrpcStreamingReaderTest, FinishImmediatelyIsIdempotent) {
  worker_queue.EnqueueBlocking(
      [&] { EXPECT_NO_THROW(reader->FinishImmediately()); });

  StartReader();

  KeepPollingGrpcQueue();
  worker_queue.EnqueueBlocking([&] {
    EXPECT_NO_THROW(reader->FinishImmediately());
    EXPECT_NO_THROW(reader->FinishAndNotify(Status::OK()));
    EXPECT_NO_THROW(reader->FinishImmediately());
  });
}

// Method prerequisites -- correct usage of `GetResponseHeaders`

TEST_F(GrpcStreamingReaderTest, CanGetResponseHeadersAfterStarting) {
  StartReader();
  EXPECT_NO_THROW(reader->GetResponseHeaders());
}

TEST_F(GrpcStreamingReaderTest, CanGetResponseHeadersAfterFinishing) {
  StartReader();

  KeepPollingGrpcQueue();
  worker_queue.EnqueueBlocking([&] {
    reader->FinishImmediately();
    EXPECT_NO_THROW(reader->GetResponseHeaders());
  });
}

// Method prerequisites -- incorrect usage

// Death tests should contain the word "DeathTest" in their name -- see
// https://github.com/google/googletest/blob/master/googletest/docs/advanced.md#death-test-naming
using GrpcStreamingReaderDeathTest = GrpcStreamingReaderTest;

TEST_F(GrpcStreamingReaderDeathTest, CannotRestart) {
  StartReader();
  KeepPollingGrpcQueue();
  worker_queue.EnqueueBlocking([&] { reader->FinishImmediately(); });
  EXPECT_DEATH_IF_SUPPORTED(StartReader(), "");
}

TEST_F(GrpcStreamingReaderTest, CannotFinishAndNotifyBeforeStarting) {
  // No callback has been assigned.
  worker_queue.EnqueueBlocking(
      [&] { EXPECT_ANY_THROW(reader->FinishAndNotify(Status::OK())); });
}

// Normal operation

TEST_F(GrpcStreamingReaderTest, OneSuccessfulRead) {
  StartReader();

  ForceFinishAnyTypeOrder({
      {Type::Write, Ok},
      {Type::Read, MakeByteBuffer("foo")},
      /*Read after last*/ {Type::Read, Error},
  });

  EXPECT_FALSE(status.has_value());

  ForceFinish({{Type::Finish, grpc::Status::OK}});

  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(status.value(), Status::OK());
  ASSERT_EQ(responses.size(), 1);
  EXPECT_EQ(ByteBufferToString(responses[0]), std::string{"foo"});
}

TEST_F(GrpcStreamingReaderTest, TwoSuccessfulReads) {
  StartReader();

  ForceFinishAnyTypeOrder({
      {Type::Write, Ok},
      {Type::Read, MakeByteBuffer("foo")},
      {Type::Read, MakeByteBuffer("bar")},
      /*Read after last*/ {Type::Read, Error},
  });
  EXPECT_FALSE(status.has_value());

  ForceFinish({{Type::Finish, grpc::Status::OK}});

  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(status.value(), Status::OK());
  ASSERT_EQ(responses.size(), 2);
  EXPECT_EQ(ByteBufferToString(responses[0]), std::string{"foo"});
  EXPECT_EQ(ByteBufferToString(responses[1]), std::string{"bar"});
}

TEST_F(GrpcStreamingReaderTest, FinishWhileReading) {
  StartReader();

  ForceFinishAnyTypeOrder({{Type::Write, Ok}, {Type::Read, Ok}});
  EXPECT_FALSE(status.has_value());

  KeepPollingGrpcQueue();
  worker_queue.EnqueueBlocking([&] { reader->FinishImmediately(); });

  EXPECT_FALSE(status.has_value());
  EXPECT_TRUE(responses.empty());
}

// Errors

TEST_F(GrpcStreamingReaderTest, ErrorOnWrite) {
  StartReader();

  bool failed_write = false;
  // Callback is used because it's indeterminate whether one or two read
  // operations will have a chance to succeed.
  ForceFinish([&](GrpcCompletion* completion) {
    switch (completion->type()) {
      case Type::Read:
        completion->Complete(true);
        break;

      case Type::Write:
        failed_write = true;
        completion->Complete(false);
        break;

      default:
        ADD_FAILURE() << "Unexpected completion type "
                      << static_cast<int>(completion->type());
        break;
    }

    return failed_write;
  });

  ForceFinish(
      {{Type::Read, Error},
       {Type::Finish, grpc::Status{grpc::StatusCode::RESOURCE_EXHAUSTED, ""}}});
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(status.value().code(), FirestoreErrorCode::ResourceExhausted);
  EXPECT_TRUE(responses.empty());
}

TEST_F(GrpcStreamingReaderTest, ErrorOnFirstRead) {
  StartReader();

  ForceFinishAnyTypeOrder({
      {Type::Write, Ok},
      {Type::Read, Error},
  });

  ForceFinish(
      {{Type::Finish, grpc::Status{grpc::StatusCode::UNAVAILABLE, ""}}});
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(status.value().code(), FirestoreErrorCode::Unavailable);
  EXPECT_TRUE(responses.empty());
}

TEST_F(GrpcStreamingReaderTest, ErrorOnSecondRead) {
  StartReader();

  ForceFinishAnyTypeOrder({
      {Type::Write, Ok},
      {Type::Read, Ok},
      {Type::Read, Error},
  });

  ForceFinish({{Type::Finish, grpc::Status{grpc::StatusCode::DATA_LOSS, ""}}});
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(status.value().code(), FirestoreErrorCode::DataLoss);
  EXPECT_TRUE(responses.empty());
}

// Callback destroys reader

TEST_F(GrpcStreamingReaderTest, CallbackCanDestroyReaderOnSuccess) {
  worker_queue.EnqueueBlocking([&] {
    reader->Start([this](const StatusOr<std::vector<grpc::ByteBuffer>>&) {
      reader.reset();
    });
  });

  ForceFinishAnyTypeOrder({
      {Type::Write, Ok},
      {Type::Read, MakeByteBuffer("foo")},
      /*Read after last*/ {Type::Read, Error},
  });

  EXPECT_NE(reader, nullptr);
  EXPECT_NO_THROW(ForceFinish({{Type::Finish, grpc::Status::OK}}));
  EXPECT_EQ(reader, nullptr);
}

TEST_F(GrpcStreamingReaderTest, CallbackCanDestroyReaderOnError) {
  worker_queue.EnqueueBlocking([&] {
    reader->Start([this](const StatusOr<std::vector<grpc::ByteBuffer>>&) {
      reader.reset();
    });
  });

  ForceFinishAnyTypeOrder({
      {Type::Write, Ok},
      {Type::Read, Error},
  });

  grpc::Status error_status{grpc::StatusCode::DATA_LOSS, ""};
  EXPECT_NE(reader, nullptr);
  EXPECT_NO_THROW(ForceFinish({{Type::Finish, error_status}}));
  EXPECT_EQ(reader, nullptr);
}

}  // namespace remote
}  // namespace firestore
}  // namespace firebase
