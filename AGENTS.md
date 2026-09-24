# Working in raPId

1. Read [PROJECT.md](PROJECT.md). It holds the working method: claim vocabulary,
   the verification gate, device rules and the definition of done.
2. Initialize the wiki with `git submodule update --init --recursive`. It is the
   manual for the product. Start with `wiki/Architecture.md`, `wiki/Building.md`
   and `wiki/Testing.md`.
3. Check the issue you are working on, including its acceptance criteria.
   Then check the actual source and device state. Don't rely on old notes.

Rules that are easy to get wrong:

- Never add AI attribution (`Co-Authored-By`, "Generated with …") to commits,
  pull requests or issues.
- Work on a branch and open a pull request. Never push to `main` or force-push.
  Stage files explicitly.
- The wiki describes current behavior only. Update the relevant page; don't
  add logs, status reports or handoff notes to it. Push the wiki before you
  commit the submodule pointer.
- Keep scratch output in the ignored `.local/`. Never commit keys, credentials,
  recordings or raw logs.
- The Pi is shared. Coordinate before touching it, and make sure the recorder
  is idle before any restart or deployment.
