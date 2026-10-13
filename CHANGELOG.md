# Changelog

## 1.0.0

First public release of VKomado.

- Renamed from SharedVD; the installer identity, driver ownership records and user settings remain compatible for in-place upgrades.

- Configurable global shortcuts for cursor return and window transfer, with persistence, disable/reset options and registration conflict feedback.
- Virtual monitor viewer with click-to-jump, cursor locator, borderless hover controls and aspect-preserving scaling.
- Live window thumbnails for moving individual windows and owned dialogs between displays.
- Window rescue on viewer exit and before disabling the managed display.
- Installer and controller using the signed Virtual Display Driver 25.7.23; pre-existing driver installations are preserved.
- New driver profiles offer 720p, 1080p, 1440p and 4K at 60 Hz, with an existing-profile migration utility.
- Vendored viewer source and third-party license notices for reproducible source builds.

Known limitations: one VDD target, no automatic placement of newly opened dialogs, no in-app resolution selector, resolution may return to 1080p after OFF/ON. Elevated applications may reject window movement. New-PC driver installation, mixed-DPI behavior and conferencing applications need broader validation.
