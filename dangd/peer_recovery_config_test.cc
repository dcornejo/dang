// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/peer_recovery_config.h"

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace dangd {
namespace {

class PeerRecoveryConfigTest : public testing::Test {
 protected:
  void SetUp() override {
    directory_ = std::filesystem::temp_directory_path() /
                 ("dang-peer-recovery-config-" + std::to_string(getpid()) +
                  "-" + std::to_string(++sequence_));
    ASSERT_TRUE(std::filesystem::create_directory(directory_));
  }

  void TearDown() override {
    std::error_code ignored;
    std::filesystem::remove_all(directory_, ignored);
  }

  std::filesystem::path Write(std::string_view name, std::string_view contents,
                              mode_t mode = S_IRUSR | S_IWUSR) {
    const std::filesystem::path path = directory_ / name;
    std::ofstream output(path, std::ios::binary);
    output << contents;
    output.close();
    EXPECT_EQ(chmod(path.c_str(), mode), 0);
    return path;
  }

  std::filesystem::path directory_;
  static inline unsigned sequence_ = 0;
};

TEST_F(PeerRecoveryConfigTest, LoadsStableTargetsAndResolvesCredentialPaths) {
  const auto path = Write("peers.json", R"json({
    "version": 1,
    "peers": [
      {"id":"primary","host":"primary.example","port":6513,
       "certificate":"client.pem","private-key":"client.key",
       "trust-anchor":"ca.pem","timeout-ms":2500},
      {"id":"standby","host":"192.0.2.20","port":6513,
       "certificate":"/etc/dangd/client.pem",
       "private-key":"/etc/dangd/client.key",
       "trust-anchor":"/etc/dangd/ca.pem"}
    ]
  })json");
  std::string error;
  const auto targets = LoadPeerRecoveryConfig(path, &error);
  ASSERT_TRUE(targets) << error;
  ASSERT_EQ(targets->size(), 2u);
  EXPECT_EQ((*targets)[0].id, "primary");
  EXPECT_EQ((*targets)[0].transport.host, "primary.example");
  EXPECT_EQ((*targets)[0].transport.certificate, directory_ / "client.pem");
  EXPECT_EQ((*targets)[0].transport.timeout_milliseconds, 2500u);
  EXPECT_EQ((*targets)[1].transport.certificate, "/etc/dangd/client.pem");
  EXPECT_EQ((*targets)[1].transport.timeout_milliseconds, 10'000u);
}

TEST_F(PeerRecoveryConfigTest, RejectsUnsafeFilesAndAmbiguousTargets) {
  const auto public_path = Write("public.json", "{\"version\":1,\"peers\":[]}",
                                 S_IRUSR | S_IWUSR | S_IRGRP);
  std::string error;
  EXPECT_FALSE(LoadPeerRecoveryConfig(public_path, &error));
  EXPECT_NE(error.find("private owned regular file"), std::string::npos);

  const auto duplicate = Write("duplicate.json", R"json({
    "version":1,"peers":[
      {"id":"same","host":"one","port":6513,"certificate":"c",
       "private-key":"k","trust-anchor":"a"},
      {"id":"same","host":"two","port":6513,"certificate":"c",
       "private-key":"k","trust-anchor":"a"}
    ]})json");
  EXPECT_FALSE(LoadPeerRecoveryConfig(duplicate, &error));
  EXPECT_NE(error.find("identity"), std::string::npos);

  const auto unknown = Write("unknown.json", R"json({
    "version":1,"peers":[
      {"id":"peer","host":"one","port":6513,"certificate":"c",
       "private-key":"k","trust-anchor":"a","typo":true}
    ]})json");
  EXPECT_FALSE(LoadPeerRecoveryConfig(unknown, &error));
  EXPECT_NE(error.find("invalid fields"), std::string::npos);
}

}  // namespace
}  // namespace dangd
