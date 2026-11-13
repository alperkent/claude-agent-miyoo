# Run your own relay (and your own build)

This fork is set up so that nothing between Claude and your Miyoo belongs to anyone else:

- **Your relay** runs on your own Cloudflare account (free plan), from `server/`.
- **Your app build** is made by GitHub Actions in this repository, from this source.
- **Updates** come from this repository's releases, not the original author's.

## What changed compared with the original

| | Original | This fork |
|---|---|---|
| Relay | the author's `claude-miyoo.miyoo-agent.workers.dev` | yours, `https://miyoo-relay.<you>.workers.dev` |
| Who can sign in to the relay | any OAuth client | only Claude (`claude.ai` / `claude.com` callbacks), see `ALLOWED_REDIRECT_HOSTS` |
| Who can register a Miyoo | anyone | at most `MAX_DEVICES` (5) Miyoos |
| KV storage id | the author's id, hard-coded | created on your account by `deploy.sh` |
| Built-in relay in the app | the author's | none (set by `server.txt` at build time, or `config/server.txt` on the SD card) |
| Updates | `cacuracaptors/claude-agent-miyoo` | this repository |

The relay protocol and the app's behaviour on the Miyoo are unchanged. `server/test/e2e.mjs` checks the whole path locally: device registration, pairing code, Claude's sign-in, tool calls, revoking, and the two new limits (`cd server && npm test`).

## 1. Publish the relay (about 15 minutes, once)

You need a computer with [Node.js](https://nodejs.org) 20 or newer and a free [Cloudflare](https://dash.cloudflare.com/sign-up) account.

```sh
git clone https://github.com/alperkent/claude-agent-miyoo
cd claude-agent-miyoo
bash server/deploy.sh
```

The script signs you in to Cloudflare in the browser, creates the storage the relay needs, publishes it and prints two addresses:

```
Relay address (for the Miyoo):  https://miyoo-relay.<you>.workers.dev
Connector address (for Claude): https://miyoo-relay.<you>.workers.dev/mcp
```

It also writes your KV id into `server/wrangler.jsonc` and the relay address into `server.txt`. Commit both so the next deploy and build use them (neither is a secret).

Optional: in GitHub, *Settings > Secrets and variables > Actions > Variables*, add `RELAY_URL` with the relay address. Builds made by GitHub Actions then use your relay without any file on the SD card.

## 2. Get your own build of the app (optional, no Docker needed)

Every push and pull request builds the app in the Miyoo Mini toolchain on GitHub Actions (the *Build* workflow; the zip is attached to the run as `ClaudeAgent`).

To publish a release the app can install as an update, push a tag equal to `AGENT_VERSION` in `src/common.h`:

```sh
git tag v1.0.1.1 && git push origin v1.0.1.1
```

## 3. Switch the Miyoo to your relay

Do these in order. Nothing here deletes anything; the old relay simply stops being used.

1. **Point the app at your relay.** Create two files in `App/ClaudeAgent/config/` on the SD card (Claude can do this for you with `write_file` while the old connector still works):
   - `server.txt` with your relay address, without `/mcp`
   - `update_repo.txt` with `alperkent/claude-agent-miyoo`
2. **Cut off the old relay.** On the Miyoo, in the Claude Agent app: *Settings > Disconnect every Claude*. This revokes every sign-in made through the author's relay. (After this, the old connector in Claude stops working.)
3. **Install your build** (if you made one in step 2): extract `ClaudeAgent-v1.0.1.1.zip` to the root of the SD card, replacing the files. Your `config/` folder, backups and settings are kept. Or, once `update_repo.txt` is in place, use *Settings > Check for updates* in the app.
4. **Restart the app** (leave it and open *Apps > Claude Agent* again). It registers with your relay and shows a new QR code, address and 6-digit code. The address on screen should be yours.
5. **Swap the connector in Claude.** In Claude, *Settings > Connectors*:
   - remove the old **Miyoo** connector (the one pointing at `claude-miyoo.miyoo-agent.workers.dev`);
   - *Add custom connector*, name it **Miyoo**, address `https://miyoo-relay.<you>.workers.dev/mcp`;
   - press *Connect* and type the 6-digit code from the Miyoo.
6. **Check it.** Start a new chat with the Miyoo connector on and ask *"what's my battery level?"*.

### What stays on the author's relay

A record of your Miyoo: a random device id, a hash of its key, the list of tools and a short summary (model, OS version, battery, free space). It cannot be used to reach the Miyoo any more: the app no longer connects there and step 2 revoked every token. There is no way to delete it from your side.

### Going back

Delete `config/server.txt` and `config/update_repo.txt` (with the original build) or put the old address in `server.txt`, restart the app and pair again.

## Settings of the relay

In `server/wrangler.jsonc`, under `vars`, then run `bash server/deploy.sh` again:

- `ALLOWED_REDIRECT_HOSTS`: sites allowed to receive the sign-in. Keep `claude.ai,claude.com` unless you want another MCP client to connect.
- `MAX_DEVICES`: how many Miyoos may register. Each restart of the app after `config/identity.json` is deleted counts as a new one.
