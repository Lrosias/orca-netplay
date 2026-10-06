// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

#include <gtest/gtest.h>

#include "Common/FileUtil.h"
#include "Common/StringUtil.h"
#include "Core/Orca/Launch.h"
#include "Core/Orca/Profile.h"

namespace
{
constexpr char GOOD[] = "# Project+ v3.2\n"
                        "[Launch]\n"
                        "Profile = PPLUS32\n"
                        "Executable = pplus/Project+ Netplay Launcher.dol\r\n";

bool Refused(const std::string& text)
{
  std::string error;
  const bool refused = !Orca::ParseLaunchIni(text, &error).has_value();
  return refused && !error.empty();
}
}  // namespace

TEST(OrcaLaunch, ParsesTheTwoKeys)
{
  std::string error;
  const auto spec = Orca::ParseLaunchIni(GOOD, &error);
  ASSERT_TRUE(spec) << error;
  EXPECT_EQ(spec->profile, "PPLUS32");
  EXPECT_EQ(spec->executable, "pplus/Project+ Netplay Launcher.dol");
}

TEST(OrcaLaunch, RefusesAnythingElse)
{
  EXPECT_TRUE(Refused(""));
  EXPECT_TRUE(Refused("[Launch]\nProfile = PPLUS32\n"));
  EXPECT_TRUE(Refused("[Launch]\nExecutable = a.dol\n"));
  EXPECT_TRUE(Refused("Profile = PPLUS32\n[Launch]\nExecutable = a.dol\n"));
  EXPECT_TRUE(Refused("[Other]\nProfile = PPLUS32\nExecutable = a.dol\n"));
  EXPECT_TRUE(Refused("[Launch]\n[Launch]\nProfile = PPLUS32\nExecutable = a.dol\n"));
  EXPECT_TRUE(Refused("[Launch]\nProfile = PPLUS32\nExecutable = a.dol\nSDImage = sd.raw\n"));
  EXPECT_TRUE(Refused("[Launch]\nProfile = PPLUS32\nProfile = RSBE01\nExecutable = a.dol\n"));
  EXPECT_TRUE(Refused("[Launch]\nProfile = PPLUS32\nExecutable\n"));
  EXPECT_TRUE(Refused("[Launch]\nProfile = ../PPLUS32\nExecutable = a.dol\n"));
  EXPECT_TRUE(Refused("[Launch]\nProfile = \nExecutable = a.dol\n"));
  EXPECT_TRUE(Refused(std::string("[Launch]\nProfile = PPLUS32\nExecutable = a.dol\n#") +
                      std::string(5000, 'x')));
}

TEST(OrcaLaunch, PathsStayInsideTheFolder)
{
  for (const char* path : {"/etc/passwd", "../a.dol", "pplus/../../a.dol", "pplus/./a.dol", "./a.dol",
                           "pplus//a.dol", "pplus/", "pplus\\a.dol", "pplus/..", ".."})
  {
    EXPECT_TRUE(Refused(std::string("[Launch]\nProfile = PPLUS32\nExecutable = ") + path + "\n"))
        << path;
  }
}

// A build folder whose name the Windows ANSI code page can't spell (paths are UTF-8 throughout).
TEST(OrcaLaunch, ReadsAFolderWithANonAsciiName)
{
  const std::string parent = File::CreateTempDir();
  const std::string dir = parent + "/Orca-é-日本-한-🎮";
  ASSERT_TRUE(File::CreateFullPath(dir + "/pplus/"));
  ASSERT_TRUE(File::WriteStringToFile(dir + "/orca-launch.ini", GOOD));
  ASSERT_TRUE(File::WriteStringToFile(dir + "/pplus/Project+ Netplay Launcher.dol", "dol"));
  std::string error;
  const auto spec = Orca::ReadLaunchIni(dir, &error);
  ASSERT_TRUE(spec) << error;
  EXPECT_NE(spec->executable.find("Orca-é-日本-한-🎮"), std::string::npos)
      << spec->executable;
  std::error_code ec;
  EXPECT_TRUE(std::filesystem::equivalent(
      StringToPath(spec->executable), StringToPath(dir) / "pplus" / "Project+ Netplay Launcher.dol",
      ec))
      << spec->executable << " " << ec.message();
  File::DeleteDirRecursively(parent);
}

TEST(OrcaLaunch, ReadsTheFileNextToTheApp)
{
  const std::string dir = File::CreateTempDir();
  std::string error;
  // No file: a plain Orca.app, not an error.
  EXPECT_FALSE(Orca::ReadLaunchIni(dir, &error));
  EXPECT_TRUE(error.empty());

  ASSERT_TRUE(File::WriteStringToFile(dir + "/orca-launch.ini", GOOD));
  // The loader isn't there yet.
  EXPECT_FALSE(Orca::ReadLaunchIni(dir, &error));
  EXPECT_FALSE(error.empty());

  ASSERT_TRUE(File::CreateFullPath(dir + "/pplus/"));
  ASSERT_TRUE(File::WriteStringToFile(dir + "/pplus/Project+ Netplay Launcher.dol", "dol"));
  const auto spec = Orca::ReadLaunchIni(dir, &error);
  ASSERT_TRUE(spec) << error;
  EXPECT_EQ(spec->profile, "PPLUS32");
  // The loader's full path (separators and drive letters differ by platform): absolute, and the
  // file written above.
  const std::filesystem::path executable = StringToPath(spec->executable);
  EXPECT_TRUE(executable.is_absolute()) << spec->executable;
  EXPECT_EQ(executable.filename().string(), "Project+ Netplay Launcher.dol");
  std::error_code ec;
  EXPECT_TRUE(std::filesystem::equivalent(
      executable, StringToPath(dir) / "pplus" / "Project+ Netplay Launcher.dol", ec))
      << spec->executable << " " << ec.message();

#ifndef _WIN32
  // A symlink that leads out of the folder is refused like a ".." path.
  const std::string outside = File::CreateTempDir();
  ASSERT_TRUE(File::WriteStringToFile(outside + "/evil.dol", "x"));
  File::Delete(dir + "/pplus/Project+ Netplay Launcher.dol");
  ASSERT_EQ(symlink((outside + "/evil.dol").c_str(),
                    (dir + "/pplus/Project+ Netplay Launcher.dol").c_str()),
            0);
  EXPECT_FALSE(Orca::ReadLaunchIni(dir, &error));
  EXPECT_NE(error.find("outside"), std::string::npos) << error;
  File::DeleteDirRecursively(outside);
#endif

  File::DeleteDirRecursively(dir);
}

// Profile.h jit_clear_frame: the JIT is cleared once per boot, at the end of that frame (the first
// boundary ends frame 0). A profile without one never clears it.
TEST(OrcaLaunch, JitClearOnceABoot)
{
  Orca::Profile profile;
  profile.jit_clear_frame = 2;
  for (int boot = 0; boot < 2; ++boot)
  {
    Orca::SetActiveProfile(profile);
    EXPECT_FALSE(Orca::JitClearDue());  // frame 0
    EXPECT_FALSE(Orca::JitClearDue());  // frame 1
    EXPECT_TRUE(Orca::JitClearDue());   // frame 2
    for (int frame = 3; frame < 1000; ++frame)
      EXPECT_FALSE(Orca::JitClearDue()) << frame;
  }
  profile.jit_clear_frame.reset();
  Orca::SetActiveProfile(profile);
  for (int frame = 0; frame < 10; ++frame)
    EXPECT_FALSE(Orca::JitClearDue()) << frame;
  Orca::SetActiveProfile(std::nullopt);
  EXPECT_FALSE(Orca::JitClearDue());
}

// Profile.h kept_code: an instruction fetch sees the game's original word only while memory holds
// the loader's word at that physical address (either MEM1 mirror), and only with such a profile.
TEST(OrcaLaunch, KeptGameCode)
{
  Orca::Profile profile;
  profile.kept_code = {{0x80023B88, 0x4182FF7C, 0x60000000}};
  Orca::SetActiveProfile(profile);
  EXPECT_EQ(Orca::KeptInstruction(0x00023B88, 0x60000000), 0x4182FF7Cu);
  EXPECT_EQ(Orca::KeptInstruction(0x00023B88, 0x4182FF7C), 0x4182FF7Cu);
  EXPECT_EQ(Orca::KeptInstruction(0x00023B88, 0x38600000), 0x38600000u);  // something else
  EXPECT_EQ(Orca::KeptInstruction(0x00023B8C, 0x60000000), 0x60000000u);
  Orca::SetActiveProfile(std::nullopt);
  EXPECT_EQ(Orca::KeptInstruction(0x00023B88, 0x60000000), 0x60000000u);
}

// PPLUS32.ini keeps Brawl's own words where Project+'s codes rewrite its code in frame 1 (ORCA.md
// "Code the JIT doesn't see"), and still clears the JIT once after they land.
TEST(OrcaLaunch, ProjectPlusKeepsBrawlsCode)
{
  const std::string sys =
      (std::filesystem::path(__FILE__).parent_path() / "../../../../Data/Sys/").string();
  std::string error;
  const std::optional<Orca::Profile> profile =
      Orca::LoadProfileFrom(sys, "PPLUS32", std::nullopt, &error);
  ASSERT_TRUE(profile) << error;
  const std::vector<Orca::Profile::KeptCode> expected = {
      {0x80023B88, 0x4182FF7C, 0x60000000}, {0x80024028, 0x4E800020, 0x481C4904},
      {0x8001CD24, 0x38631198, 0x806311A0}, {0x8001CD2C, 0x80630008, 0x38630006},
      {0x800266B8, 0x38672CAC, 0x38672CB2}, {0x8018CFC4, 0x2C000000, 0x28000030},
      {0x8018CFC8, 0x41820034, 0x41800034},
  };
  EXPECT_EQ(profile->kept_code, expected);
  EXPECT_EQ(profile->jit_clear_frame, 2u);
}
