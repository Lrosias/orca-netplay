// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <vector>

#include <gtest/gtest.h>

#include "Common/CommonTypes.h"
#include "Core/Orca/PanicDiag.h"

using Orca::PanicDiag::DescribeWord;
using Orca::PanicDiag::View;

namespace
{
// Low MEM1 as two host views see it, big-endian like the guest's.
std::array<u8, 0x1000> Ram(u32 word_at_500)
{
  std::array<u8, 0x1000> ram{};
  ram[0x500] = static_cast<u8>(word_at_500 >> 24);
  ram[0x501] = static_cast<u8>(word_at_500 >> 16);
  ram[0x502] = static_cast<u8>(word_at_500 >> 8);
  ram[0x503] = static_cast<u8>(word_at_500);
  return ram;
}
}  // namespace

TEST(OrcaPanicDiag, ViewsThatAgree)
{
  const auto ram = Ram(0x7c7b43a6);
  const std::vector<View> views{{"ram", ram.data(), 0, 0x1000},
                                {"logical:80000000", ram.data(), 0, 0x1000}};
  EXPECT_EQ(DescribeWord(0x500, views),
            "orca diag word 00000500: ram=7c7b43a6 logical:80000000=7c7b43a6");
}

TEST(OrcaPanicDiag, ViewsThatDisagreeSaySo)
{
  const auto ram = Ram(0);
  const auto logical = Ram(0x7c7b43a6);
  const std::vector<View> views{{"ram", ram.data(), 0, 0x1000},
                                {"logical:80000000", logical.data(), 0, 0x1000}};
  EXPECT_EQ(DescribeWord(0x500, views),
            "orca diag word 00000500: ram=00000000 logical:80000000=7c7b43a6 differ");
}

TEST(OrcaPanicDiag, OnlyViewsThatCoverTheWord)
{
  const auto mem1 = Ram(0x12345678);
  const std::vector<View> views{{"exram", mem1.data(), 0x10000000, 0x1000},
                                {"ram", mem1.data(), 0, 0x1000},
                                {"short", mem1.data(), 0, 0x502},
                                {"null", nullptr, 0, 0x1000}};
  EXPECT_EQ(DescribeWord(0x500, views), "orca diag word 00000500: ram=12345678");
  EXPECT_EQ(DescribeWord(0x20000000, views), "orca diag word 20000000: no view");
}

TEST(OrcaPanicDiag, ReadsTheVectorsAndThreadGlobals)
{
  // The external-interrupt vector and the OS's current-thread pointer are what the Mac crash read
  // as zero.
  const auto& words = Orca::PanicDiag::WORDS;
  EXPECT_NE(std::find(words.begin(), words.end(), 0x500u), words.end());
  EXPECT_NE(std::find(words.begin(), words.end(), 0xE4u), words.end());
}
