# macOS unsigned app notes

SeqEyes provides unsigned macOS app bundle zips on the GitHub Releases page:

- `seqeyes-<version>-macos-arm64.zip` for Apple Silicon Macs.
- `seqeyes-<version>-macos-x86_64.zip` for Intel Macs.

The recommended macOS installation method is still:

```bash
pip install seqeyes
```

The app bundle zip is provided for users who prefer a downloadable application bundle. It is not signed with an Apple Developer ID certificate and is not notarized by Apple, so macOS Gatekeeper may block the first launch.

## Install from the unsigned app zip

1. Download the correct macOS zip from the [SeqEyes releases page](https://github.com/xingwangyong/seqeyes/releases).
2. Unzip it.
3. Move `seqeyes.app` to `/Applications` or another folder where you keep applications.
4. Try opening `seqeyes.app`.

If macOS reports that the app cannot be opened because the developer cannot be verified:

1. Open Apple menu > System Settings > Privacy & Security.
2. In the Security section, find the blocked SeqEyes launch message.
3. Click Open Anyway.
4. Confirm that you want to open the app.

After this one-time approval, macOS should allow future launches normally.

## Alternative launch method

You can also Control-click `seqeyes.app`, choose Open, and confirm the warning dialog if macOS offers that option.
