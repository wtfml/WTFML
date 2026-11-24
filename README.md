<p align="center"><b>English</b> | <a href="README.zh-CN.md">简体中文</a></p>

<p align="center">
  <img src="assets/readme/hero.webp" alt="WTFML · Wonderful Tremendous Fantastic Minecraft Launcher" width="100%">
</p>

<p align="center">An elegant, beautiful, all-in-one, modern Minecraft launcher.</p>

<p align="center">
  <a href="https://github.com/wtfml/WTFML/releases/latest"><img src="assets/readme/download-en.svg" alt="Download" height="48"></a>
</p>

<p align="center"><sub>A single exe — download and run, nothing to install. Available for Windows x64.</sub></p>

<br>

## <img src="assets/readme/h-home-en.svg" alt="Home" height="40">

<p align="center">
  <img src="assets/readme/shot-home-en.webp" alt="Home page: greeting, quote of the day, current instance and Play button" width="100%">
</p>

## <img src="assets/readme/h-features-en.svg" alt="Features" height="40">

- **All in one** — Vanilla, Fabric, Quilt, Forge and NeoForge, plus Paper, Purpur and Spigot servers.
- **Accounts** — Microsoft or offline; keep several, switch freely, change skins and capes.
- **Modpacks** — Import ZIP, mrpack, CurseForge or MultiMC / Prism instances; RAR and 7z too with 7-Zip.
- **Instances** — Group and favorite; mods, resource packs, shaders, data packs, worlds and servers in one place.
- **Safe upgrades** — Move to a new game version as a copy, or in place behind a verified full snapshot you can roll back to.
- **Fast downloads** — Concurrent, resumable, speed limit and proxy; existing files verified and reused.
- **Java** — Finds installed Java or fetches one from Adoptium.
- **Servers** — Console, `server.properties` editor, whitelist / ops / bans, backups.
- **Toolbox** — Enchanting order, coordinates, beacons, brewing, banners, colored text, server ping and more.
- **Agent** — Connect an OpenAI-compatible model to read logs and crash reports with you; nothing changes without your approval.
- **Looks** — Three scenic themes over frosted glass; your own background and accent color.

<details>
<summary><b>More screenshots</b></summary>

<br>

<table>
  <tr>
    <td width="50%"><img src="assets/readme/shot-library-en.webp" alt="Library page"><br><sub>Library — instances and groups</sub></td>
    <td width="50%"><img src="assets/readme/shot-download-en.webp" alt="Download page"><br><sub>Download — game versions and loaders</sub></td>
  </tr>
  <tr>
    <td width="50%"><img src="assets/readme/shot-settings-en.webp" alt="Settings page"><br><sub>Settings — changes save as you go</sub></td>
    <td width="50%"><img src="assets/readme/shot-toolbox-en.webp" alt="Toolbox"><br><sub>Toolbox — calculators and helpers for play</sub></td>
  </tr>
  <tr>
    <td width="50%"><img src="assets/readme/shot-agent-en.webp" alt="Agent page"><br><sub>Agent — chat about the current instance</sub></td>
    <td width="50%"><img src="assets/readme/themes-en.webp" alt="The home page in all three themes"><br><sub>Themes — the same home page in all three</sub></td>
  </tr>
</table>

</details>

<details>
<summary><b>FAQ</b></summary>

<br>

**Where is my data stored?**

In the game folder you have selected, wherever the exe lives. The launcher's own settings and account list are in the registry under `HKCU\Software\WTFML`; sign-in tokens are encrypted with Windows DPAPI before they touch the disk.

**Where is the log?**

`<game folder>/logs/wtfml.log`, rewritten on every start. The game's own output goes to a separate log window, not this file.

**Can I open two windows?**

One launcher window per game folder at a time; a second one is refused before it opens. A different game folder works fine.

**Where are agent conversations stored?**

`%LOCALAPPDATA%\WTFML\agent`, independent of the game folder. Each conversation can be stored encrypted with DPAPI.

</details>

## <img src="assets/readme/h-contributing-en.svg" alt="Contributing" height="40">

Issues and pull requests are welcome. When reporting a problem, attaching `logs/wtfml.log` saves a lot of back-and-forth. The developer docs are written in Chinese.

- Building, running and source layout: [docs/dev/BUILDING.md](docs/dev/BUILDING.md)
- Running the tests: [docs/dev/TESTING.md](docs/dev/TESTING.md)
- Documentation index: [docs/README.md](docs/README.md)
- Repository rules (layout, docs, commits): [AGENTS.md](AGENTS.md)

## <img src="assets/readme/h-license-en.svg" alt="License" height="40">

WTFML is released under the [GNU GPL v3](LICENSE). Dancing Script and LXGW WenKai, used in the interface, are under the SIL Open Font License. The theme landscapes come from Wikimedia Commons; authors and licenses are listed in [assets/scenes/README.md](assets/scenes/README.md).
