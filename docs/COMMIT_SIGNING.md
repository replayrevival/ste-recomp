# Commit signing

Commits and tags in this repository are signed with an SSH key, so GitHub shows them as **Verified**.

- The signing key is registered on the project's GitHub account as a *signing key* (public part only).
- Repository-local settings (not global, so other projects on the same PC are unaffected): `user.name` "Replay Revival", `user.email` the account's GitHub noreply address, `gpg.format ssh`, `user.signingkey` the key file, `commit.gpgsign true`, `tag.gpgsign true`.
- Check a commit locally with `git log --show-signature -1` (needs `gpg.ssh.allowedSignersFile`, a file with the noreply address and the public key).
- History made before signing was set up stays unsigned; it was not rewritten.
- If a commit is made without a signature, do not push it: amend it with `git commit --amend -S --no-edit` first (only while it is still local).
