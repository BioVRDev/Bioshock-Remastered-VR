Binaries the build does not produce.

Put these two files here, from their upstream sources, so a release can be
assembled without hunting for them:

  openxr_loader_standard.dll   the official Khronos 32-bit OpenXR loader
  openvr_api.dll               Valve's OpenVR SDK, bin/win32/

They are NOT built from this repo. Everything else in a release comes out of
Release/ -- see FILES.md for the full set and what breaks without each one.

They are gitignored: they are third-party redistributables with their own
licences, and vendoring binaries into the repo is a separate decision. FILES.md
records exactly what belongs here so the absence is documented rather than
surprising.
