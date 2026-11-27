// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#define PSIOPS_VERSION_MAJOR 1
#define PSIOPS_VERSION_MINOR 2
#define PSIOPS_VERSION_PATCH 0

#ifndef PSIOPS_VERSION_STR
#define PSIOPS_VERSION_STR "1.2.0"
#endif
#define PSIOPS_VERSION_TAG "v" PSIOPS_VERSION_STR

#define PSIOPS_WIDE_IMPL(s) L##s
#define PSIOPS_WIDE(s) PSIOPS_WIDE_IMPL(s)
#define PSIOPS_WVERSION_STR PSIOPS_WIDE(PSIOPS_VERSION_STR)
#define PSIOPS_WVERSION_TAG PSIOPS_WIDE(PSIOPS_VERSION_TAG)

#define PSIOPS_GITHUB_REPO "Suvukaraz/Psi-Ops-The-Mindgate-Conspiracy-ReComp"
#define PSIOPS_REPO_URL "https://github.com/" PSIOPS_GITHUB_REPO
#define PSIOPS_RELEASES_URL PSIOPS_REPO_URL "/releases"
#define PSIOPS_USER_AGENT "Psi-Ops-The-Mindgate-Conspiracy-ReComp/" PSIOPS_VERSION_STR

#ifdef PSIOPS_CMAKE_VER_MAJOR
static_assert(PSIOPS_VERSION_MAJOR == PSIOPS_CMAKE_VER_MAJOR &&
                  PSIOPS_VERSION_MINOR == PSIOPS_CMAKE_VER_MINOR &&
                  PSIOPS_VERSION_PATCH == PSIOPS_CMAKE_VER_PATCH,
              "Keep src/common/version.h synchronized with CMakeLists.txt project(PsiOps VERSION ...)");
#endif
