// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/plugin_worker_protocol.h"

#include <array>
#include <chrono>
#include <string>
#include <thread>

#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

namespace dangd {
namespace {

using namespace std::chrono_literals;

class SocketPair {
 public:
  SocketPair() { EXPECT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets_.data()), 0); }
  ~SocketPair() {
    for (const int descriptor : sockets_)
      if (descriptor >= 0) close(descriptor);
  }
  int Take(std::size_t index) {
    const int descriptor = sockets_[index];
    sockets_[index] = -1;
    return descriptor;
  }

 private:
  std::array<int, 2> sockets_{-1, -1};
};

TEST(PluginWorkerProtocolTest, ExchangesBoundedFrames) {
  SocketPair sockets;
  const int writer = sockets.Take(0);
  const int reader = sockets.Take(1);
  std::thread peer([writer] {
    std::string error;
    EXPECT_EQ(WriteWorkerFrame(writer, "worker-response",
                               std::chrono::steady_clock::now() + 1s, 1024,
                               &error),
              WorkerIoStatus::kOk) << error;
    close(writer);
  });
  const WorkerReadResult result = ReadWorkerFrame(
      reader, std::chrono::steady_clock::now() + 1s, 1024);
  EXPECT_EQ(result.status, WorkerIoStatus::kOk) << result.error;
  EXPECT_EQ(result.payload, "worker-response");
  close(reader);
  peer.join();
}

TEST(PluginWorkerProtocolTest, TimesOutWithoutAWorkerResponse) {
  SocketPair sockets;
  const int idle = sockets.Take(0);
  const int reader = sockets.Take(1);
  const WorkerReadResult result = ReadWorkerFrame(
      reader, std::chrono::steady_clock::now() + 20ms, 1024);
  EXPECT_EQ(result.status, WorkerIoStatus::kTimeout);
  close(idle);
  close(reader);
}

TEST(PluginWorkerProtocolTest, DistinguishesCleanExitFromTruncatedFrame) {
  {
    SocketPair sockets;
    const int peer = sockets.Take(0);
    const int reader = sockets.Take(1);
    close(peer);
    EXPECT_EQ(ReadWorkerFrame(reader, std::chrono::steady_clock::now() + 1s,
                              1024).status,
              WorkerIoStatus::kPeerClosed);
    close(reader);
  }
  {
    SocketPair sockets;
    const int peer = sockets.Take(0);
    const int reader = sockets.Take(1);
    const unsigned char partial[] = {0, 0, 0};
    ASSERT_EQ(send(peer, partial, sizeof(partial), 0),
              static_cast<ssize_t>(sizeof(partial)));
    close(peer);
    const WorkerReadResult result = ReadWorkerFrame(
        reader, std::chrono::steady_clock::now() + 1s, 1024);
    EXPECT_EQ(result.status, WorkerIoStatus::kProtocolError);
    EXPECT_NE(result.error.find("partial frame header"), std::string::npos);
    close(reader);
  }
  {
    SocketPair sockets;
    const int peer = sockets.Take(0);
    const int reader = sockets.Take(1);
    const unsigned char partial[] = {0, 0, 0, 4, 'o', 'k'};
    ASSERT_EQ(send(peer, partial, sizeof(partial), 0),
              static_cast<ssize_t>(sizeof(partial)));
    close(peer);
    const WorkerReadResult result = ReadWorkerFrame(
        reader, std::chrono::steady_clock::now() + 1s, 1024);
    EXPECT_EQ(result.status, WorkerIoStatus::kProtocolError);
    EXPECT_NE(result.error.find("partial frame payload"), std::string::npos);
    close(reader);
  }
}

TEST(PluginWorkerProtocolTest, RejectsOversizedFramesBeforeAllocation) {
  SocketPair sockets;
  const int peer = sockets.Take(0);
  const int reader = sockets.Take(1);
  const unsigned char header[] = {0, 0, 4, 1};
  ASSERT_EQ(send(peer, header, sizeof(header), 0),
            static_cast<ssize_t>(sizeof(header)));
  const WorkerReadResult result = ReadWorkerFrame(
      reader, std::chrono::steady_clock::now() + 1s, 1024);
  EXPECT_EQ(result.status, WorkerIoStatus::kProtocolError);
  EXPECT_TRUE(result.payload.empty());
  EXPECT_NE(result.error.find("payload limit"), std::string::npos);
  close(peer);
  close(reader);
}

TEST(PluginWorkerProtocolTest, RejectsOversizedWritesWithoutTouchingSocket) {
  SocketPair sockets;
  const int writer = sockets.Take(0);
  const int reader = sockets.Take(1);
  std::string error;
  EXPECT_EQ(WriteWorkerFrame(writer, std::string(1025, 'x'),
                             std::chrono::steady_clock::now() + 1s, 1024,
                             &error),
            WorkerIoStatus::kProtocolError);
  EXPECT_NE(error.find("payload limit"), std::string::npos);
  const WorkerReadResult result = ReadWorkerFrame(
      reader, std::chrono::steady_clock::now() + 20ms, 1024);
  EXPECT_EQ(result.status, WorkerIoStatus::kTimeout);
  close(writer);
  close(reader);
}

}  // namespace
}  // namespace dangd
