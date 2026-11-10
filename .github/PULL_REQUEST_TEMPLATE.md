## Summary

<!-- What does this PR change and why? Keep it short. -->

## Tests

- [ ] Added/updated unit tests in `tests/src/test_*.cpp` for new behavior — required for any parser or untrusted-input change
- [ ] New file format or untrusted-input path has a libFuzzer target in `tests/fuzz/` plus corpus seeds in `tests/fuzz/corpus/`
- [ ] `cmake --preset headless && cmake --build --preset headless && ctest --preset headless` passes locally
- [ ] If graphics code is touched: `cmake --preset full && cmake --build --preset full && ctest --preset full` passes locally

## Security

- [ ] No secrets, tokens, or credentials in the diff (gitleaks CI scans full history)
- [ ] Dependencies stay pinned: every `FetchContent` `GIT_TAG` is a 40-hex commit SHA over `https://` (dep-pin-audit enforces this)
- [ ] Every GitHub Action is pinned to a full commit SHA (actions-pin-audit enforces this)
- [ ] Malformed/untrusted input fails with controlled errors: no exceptions across module boundaries, no unbounded allocations, no host-FS access in `PackageOnly`/project modes
- [ ] CI, Security CI, and CodeQL jobs are green on this PR

## Notes

<!-- Anything reviewers should know: behavior changes, migration, follow-ups. -->
