# The project version, alone in its own file.
#
# Not in CMakeLists.txt, and that is the entire point: CMakeLists.txt is a
# frozen architecture surface, so bumping a version there turned the freeze
# gate red on every release and demanded an approved finding for a change that
# is by definition not architectural. The alternative was to teach the gate a
# semantic exception; a gate that has learned to be persuaded once is easier to
# persuade again, so the fact was moved instead of the rule.
#
# Three components, SemVer. The Windows resource and the manifest carry the
# same version as four fields (0.13.0.0) because those formats require four;
# the fourth is build/tweak and is always 0 here. Both are GENERATED from this
# line -- see src/resources/gtg_version.h.in and
# src/resources/GpuThermalGuard.manifest.in -- so they cannot disagree with it.
#
# To release: change this line and add a CHANGELOG.md section. That is the
# whole list.
set(GTG_VERSION 0.13.0)
