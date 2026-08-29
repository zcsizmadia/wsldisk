## What this changes

<!-- One or two sentences. Link the roadmap item or issue: "Closes #12". -->

## Why

<!-- The user-visible problem. For anything under ops/ or platform/, say what
     could go wrong and how this change avoids it. -->

## How it was tested

<!-- Which suites ran, and anything you exercised by hand. Integration runs need
     WSLDISK_INTEGRATION=1 and a throwaway distro. -->

## Checklist

- [ ] Unit tests cover every new branch (`ctest --preset x64-debug`)
- [ ] Contract or integration test added if this touches `platform/` or a command
- [ ] Coverage still 100% (`cmake --build --preset x64-coverage-gate`)
- [ ] Golden output updated if user-visible output changed
- [ ] `docs/` updated if behaviour changed
- [ ] Every new error message carries a remedy
- [ ] `clang-format` clean; Conventional Commit title
