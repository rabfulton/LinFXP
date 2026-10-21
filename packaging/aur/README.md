# AUR package

`linfxp-git` builds the latest default-branch commit from GitHub. Its version
combines `VERSION`, the commit count, and the abbreviated commit hash.
`SKIP` is used for the changing Git source, not a fixed release archive.

## Try the recipe

From the project root, copy the recipe into an ignored build directory:

```sh
mkdir -p build/aur
cp packaging/aur/PKGBUILD build/aur/
cd build/aur
makepkg -si
```

Run as your normal user. `makepkg -si` uses sudo to install missing dependencies
and the resulting package. It replaces the locally installed `linfxp` package;
saved connections and credentials are preserved. Tests use localhost SSH/SFTP
sockets and do not contact your saved servers.

## Submit to the AUR

First push the desired source changes to GitHub; this recipe does not build
uncommitted local changes. Add an email address to the Maintainer comment if
you want one published. Create an AUR account and register your SSH public key.
Then, in a separate directory outside this project:

```sh
git clone ssh://aur@aur.archlinux.org/linfxp-git.git
cd linfxp-git
cp /path/to/LinFXP/packaging/aur/PKGBUILD .
makepkg -s
makepkg --printsrcinfo > .SRCINFO
namcap PKGBUILD
git add PKGBUILD .SRCINFO
git commit -m "Add linfxp-git"
git push
```

Commit only the recipe and `.SRCINFO` to the AUR, not application source or built
packages. Regenerate `.SRCINFO` whenever the recipe metadata changes. Git
packages follow new commits when rebuilt; AUR helpers may need their development
package update option (for example, `yay -Syu --devel`).

The recipe has not been submitted automatically. For a stable `linfxp` package,
publish a versioned release tag and use its archive with a real SHA-256 checksum.

References: [VCS packaging](https://wiki.archlinux.org/title/VCS_package_guidelines),
[AUR submissions](https://wiki.archlinux.org/title/AUR_submission_guidelines),
and [.SRCINFO](https://wiki.archlinux.org/title/.SRCINFO).
