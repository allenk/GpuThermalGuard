#include "color_transition_scope.hpp"
#include "output_diagnostics.hpp"
#include "fresh_output.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

void Check(bool value, const char* text) {
    if (!value) {
        std::fprintf(stderr, "FAIL %s\n", text);
        std::exit(1);
    }
}

int main(int argc, char** argv) {
    using namespace gtg::overlay::integration;
    if (argc == 2 && std::strcmp(argv[1], "--nested") == 0) {
        unsigned calls{}, failures{};
        Check(ForwardNestedColor(
                  true,
                  [&]() noexcept {
                      ++calls;
                      return S_FALSE;
                  },
                  [&]() noexcept { ++failures; }) == S_FALSE &&
                  calls == 1 && failures == 0,
              "successful nested SDR conserves original result");
        Check(ForwardNestedColor(
                  true,
                  [&]() noexcept {
                      ++calls;
                      return E_INVALIDARG;
                  },
                  [&]() noexcept { ++failures; }) == E_INVALIDARG &&
                  calls == 2 && failures == 1,
              "failed nested SDR must close outer generation");
        Check(ForwardNestedColor(
                  false,
                  [&]() noexcept {
                      ++calls;
                      return S_OK;
                  },
                  [&]() noexcept { ++failures; }) == S_OK &&
                  calls == 3 && failures == 2,
              "nested non-SDR remains fatal without changing original result");
    } else if (argc == 1) {
        Check(TrackColorSetter(0, 123), "post-install preselection setter must be tracked");
        Check(TrackColorSetter(123, 123) && !TrackColorSetter(123, 456),
              "selected identity distinguishes unrelated chains");
    } else {
        struct Fresh {
            bool window = true, create = true, current = true, same_monitor = true;
            bool output6 = true, description = true, sdr = true;
            LONG find = 1;
            unsigned parent_calls{}, creates{};

            bool ParentCurrent() noexcept {
                ++parent_calls;
                return false;
            }

            bool Window() noexcept { return window; }

            bool Create() noexcept {
                ++creates;
                return create;
            }

            bool Current() noexcept { return current; }

            LONG Find() noexcept { return find; }

            bool Output6() noexcept { return output6; }

            bool Description() noexcept { return description; }

            bool Sdr() noexcept { return sdr; }

            bool SameMonitor() noexcept { return same_monitor; }
        } fresh;

        Check(QualifyFreshOutput(fresh) == 1 && fresh.parent_calls == 0 && fresh.creates == 1,
              "stale game factory cannot reject independently qualified fresh SDR output");
        fresh.sdr = false;
        Check(QualifyFreshOutput(fresh) == -265, "fresh factory never bypasses non-SDR output");
        fresh.sdr = true;
        fresh.current = false;
        Check(QualifyFreshOutput(fresh) == -261, "fresh factory stale still refuses");
        fresh.current = true;
        fresh.same_monitor = false;
        Check(QualifyFreshOutput(fresh) == -284, "window moving during output query refuses");
        fresh.same_monitor = true;
        fresh.find = -283;
        Check(QualifyFreshOutput(fresh) == -283, "missing monitor never falls back to primary");
        fresh.find = 1;
        fresh.create = false;
        Check(QualifyFreshOutput(fresh) == -280, "fresh factory creation failure refuses");
        fresh = {};
        fresh.window = false;
        Check(QualifyFreshOutput(fresh) == -281 && fresh.creates == 0,
              "missing HWND monitor never creates a factory");
        fresh = {};
        fresh.output6 = false;
        Check(QualifyFreshOutput(fresh) == -263, "missing Output6 cannot establish SDR");
        fresh = {};
        fresh.description = false;
        Check(QualifyFreshOutput(fresh) == -264,
              "failed extended description cannot establish SDR");

        struct Enumeration {
            UINT adapters = 2, outputs = 2, match_adapter = 1, match_output = 1;
            UINT active_adapter{}, active_output{}, adapter_calls{}, output_calls{};
            bool adapter_error{}, output_error{}, description_error{};

            HRESULT Adapter(UINT index) noexcept {
                ++adapter_calls;
                active_adapter = index;
                return adapter_error ? E_FAIL : index < adapters ? S_OK : DXGI_ERROR_NOT_FOUND;
            }

            HRESULT Output(UINT index) noexcept {
                ++output_calls;
                active_output = index;
                return output_error ? E_FAIL : index < outputs ? S_OK : DXGI_ERROR_NOT_FOUND;
            }

            bool OutputDescription() noexcept { return !description_error; }

            bool MatchesMonitor() noexcept {
                return active_adapter == match_adapter && active_output == match_output;
            }
        } enumeration;

        Check(FindFreshMonitorOutput(enumeration) == 1 && enumeration.adapter_calls == 2 &&
                  enumeration.output_calls == 5,
              "fresh enumeration selects monitor on non-primary adapter");
        enumeration = {};
        enumeration.match_adapter = 99;
        Check(FindFreshMonitorOutput(enumeration) == -283,
              "unmatched monitor refuses after enumeration end");
        enumeration = {};
        enumeration.outputs = 100;
        enumeration.match_adapter = 99;
        Check(FindFreshMonitorOutput(enumeration) == -285 &&
                  enumeration.output_calls == kOutputPerAdapterLimit,
              "output scan is bounded and limit exhaustion refuses");
        enumeration = {};
        enumeration.adapters = 100;
        enumeration.outputs = 0;
        Check(FindFreshMonitorOutput(enumeration) == -285 &&
                  enumeration.adapter_calls == kOutputAdapterLimit,
              "adapter scan is bounded even without any output");
        enumeration = {};
        enumeration.adapter_error = true;
        Check(FindFreshMonitorOutput(enumeration) == -282 && enumeration.output_calls == 0,
              "failed adapter query stops enumeration");
        enumeration = {};
        enumeration.output_error = true;
        Check(FindFreshMonitorOutput(enumeration) == -282, "failed output query stops enumeration");
        enumeration = {};
        enumeration.description_error = true;
        Check(FindFreshMonitorOutput(enumeration) == -264,
              "failed monitor descriptor cannot select output");

        struct Output {
            unsigned fail{}, calls{};

            bool Step(unsigned index) noexcept {
                ++calls;
                return fail != index;
            }

            bool Parent() noexcept { return Step(1); }

            bool Current() noexcept { return Step(2); }

            bool Containing() noexcept { return Step(3); }

            bool Output6() noexcept { return Step(4); }

            bool Description() noexcept { return Step(5); }

            bool Sdr() noexcept { return Step(6); }
        };

        for (unsigned step = 1; step <= 6; ++step) {
            Output output{step};
            Check(
                DiagnoseSdrOutput(output) == -259 - static_cast<LONG>(step) && output.calls == step,
                "output refusal identifies exact first failed step without later calls");
        }
        Output healthy;
        Check(DiagnoseSdrOutput(healthy) == 1 && healthy.calls == 6,
              "healthy output preserves all six checks");
        const bool states[][4] = {{false, true, true, false}, {true, true, false, false},
                                  {true, true, true, true},   {true, true, true, false},
                                  {true, true, true, false},  {true, false, true, false}};
        const LONG expected[] = {-251, -270, -271, -263, -272, 1};
        for (unsigned i = 0; i < 6; ++i) {
            unsigned output_calls{}, rebuild_calls{};
            const auto& s = states[i];
            const LONG result = DiagnoseOutputGeneration(
                s[0], s[1], s[2], s[3],
                [&]() noexcept {
                    ++output_calls;
                    return i == 3 ? -263L : 1L;
                },
                [&](bool valid) noexcept {
                    ++rebuild_calls;
                    Check(valid == (i == 4), "rebuild gets exact qualification");
                    return false;
                });
            Check(result == expected[i],
                  "transition preserves original, blocked, output or rebuild cause");
            Check(output_calls == (i >= 3 ? 1U : 0U), "failed predecessor never probes output");
            Check(rebuild_calls == ((s[0] && s[1]) ? 1U : 0U),
                  "drained initialized generation always rebuilt or aborted");
        }
        unsigned probes{}, rebuilds{};
        const bool result = RequalifyOutputGeneration(
            true, true, true, false,
            [&]() noexcept {
                ++probes;
                return false;
            },
            [&](bool qualified) noexcept {
                ++rebuilds;
                Check(!qualified, "changed output aborts private generation");
                return qualified;
            });
        Check(!result && probes == 1 && rebuilds == 1,
              "changed output prevents reopening after resize/fullscreen");
        probes = rebuilds = 0;
        Check(RequalifyOutputGeneration(
                  true, true, true, false,
                  [&]() noexcept {
                      ++probes;
                      return true;
                  },
                  [&](bool qualified) noexcept {
                      ++rebuilds;
                      return qualified;
                  }) &&
                  probes == 1 && rebuilds == 1,
              "healthy output is rechecked before rebuilding");
        Check(!RequalifyOutputGeneration(
                  true, true, false, false,
                  [&]() noexcept {
                      Check(false, "failed original cannot probe output");
                      return true;
                  },
                  [&](bool qualified) noexcept {
                      Check(!qualified, "failed original aborts resources");
                      return false;
                  }),
              "failure stays closed");
    }
    std::puts("PASS color transition scope/output qualification");
}
