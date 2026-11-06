#pragma once

#include <optional>
#include <string>

#include "update/hosts_updates.hpp"
#include "update/hosts_wire.hpp"

// What the Hosts dialog says about updates, in the reader's language: its two
// columns, its status line and its confirmations.
namespace gtg::update {

// "1.2.1 FEB2ED5C", or the version alone while the build is unknown.
[[nodiscard]] std::wstring VersionCell(const HostView& host);
// What the host is doing, or what came of the last request to it.
[[nodiscard]] std::wstring UpdateCell(const HostsUpdates& updates, const HostView& host);
// An answer as a sentence.
[[nodiscard]] std::wstring AnswerText(hosts::Answer answer);
// The release this machine's source names.
[[nodiscard]] std::wstring SourceLine(const std::optional<SourceView>& source);
// "Update all", or the selected host's last answer.
[[nodiscard]] std::wstring ProgressLine(const HostsUpdates& updates, std::uint64_t selected);
[[nodiscard]] std::wstring ConfirmText(const HostView& host, const SourceView& source, bool force);
[[nodiscard]] std::wstring ConfirmAllText(const HostsUpdates& updates, const SourceView& source);

}  // namespace gtg::update
