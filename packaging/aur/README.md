# AUR packaging

Canonical copy of the AUR `PKGBUILD`. The AUR itself is a separate git
remote; to publish or update:

```sh
git clone ssh://aur@aur.archlinux.org/c64uv.git aur-c64uv
cp packaging/aur/PKGBUILD aur-c64uv/
cd aur-c64uv
makepkg --printsrcinfo > .SRCINFO
git add PKGBUILD .SRCINFO
git commit -m "Update to <version>"
git push
```

On each release: bump `pkgver`, reset `pkgrel` to 1, refresh `sha256sums`
(`updpkgsums` or `sha256sum` of the tag tarball), test with `makepkg -f`,
then push both here and to the AUR.

First-time AUR setup: an AUR account with your SSH public key added
(https://aur.archlinux.org, My Account), then the clone above creates the
package on first push.
