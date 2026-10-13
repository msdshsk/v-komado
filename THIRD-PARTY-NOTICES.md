# Third-party notices

SharedVD's original code is licensed under the MIT License; see LICENSE.

## Windows Virtual Monitor Loopback

The viewer in `viewer/` is derived from
https://github.com/giri256/windows-virtual-monitor-loopback
at commit `b15c119684bd6d718a5353395b0213d7bccf76d0`.

Copyright (c) 2026 giri256. MIT License.
The full license is preserved in `viewer/LICENSE` and
`third_party/licenses/Loopback-MIT.txt` (portable packages: `Loopback-MIT.txt`;
installed packages: `Licenses/Loopback-MIT.txt`).

SharedVD changes include cursor jumping instead of input forwarding,
borderless hover controls, frame pacing, window rescue, window selection,
configurable shortcuts, application branding and static runtime linking.

## Virtual Display Driver

The setup package redistributes the unmodified signed x64 driver from release
25.7.23 of https://github.com/VirtualDrivers/Virtual-Display-Driver.
The driver archive is downloaded separately and is not stored in Git.

Copyright (c) 2024 Virtual Display. MIT License.
See `third_party/licenses/VDD-MIT.txt` (installed packages: `Licenses/VDD-MIT.txt`).

## Build tools and assets

Inno Setup is a separately obtained build tool, not a source dependency
vendored in this repository. Its applicable licensing terms are at
https://jrsoftware.org/isinfo.php and https://jrsoftware.org/isorder.php.

The SharedVD icon was generated with OpenAI image generation.
Asset and conversion information is recorded in `assets/ICON.txt`.
