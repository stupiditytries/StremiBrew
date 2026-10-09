<p align="center">
  <img src="docs/images/stremibrew-logo.png" alt="StremiBrew" width="460">
</p>

<p align="center">
  <a href="https://github.com/stupiditytries/StremiBrew/actions/workflows/build.yml"><img src="https://github.com/stupiditytries/StremiBrew/actions/workflows/build.yml/badge.svg?branch=main" alt="Build status"></a>&nbsp;
  <img src="https://img.shields.io/badge/Tokens%20Wasted-9%20Kazillion%2B-blueviolet" alt="Tokens Wasted: 9 Kazillion+">&nbsp;
  <a href="LICENSE.md"><img src="https://img.shields.io/badge/license-MIT-blue" alt="License: MIT"></a>&nbsp;
  <img src="https://img.shields.io/badge/built%20with-C%2B%2B20%20%7C%20Rust-8B5A2B" alt="Built with C++20 and Rust">
</p>

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/images/stremibrew-dark.png">
    <img src="docs/images/stremibrew-light.png" alt="StremiBrew on a TV and a Nintendo Switch" width="100%">
  </picture>
</p>

<hr>

<h3 align="center">Cross-Platform Unofficial Stremio Client</h3>

<hr>

StremiBrew is a cross-platform, third-party Stremio client that aims to bring the native Stremio TV experience to consoles and builds on it with features only console hardware can handle.

The UI is inspired by my favourite elements of Stremio's official web-based TV interface and its Android TV app.

It currently supports the **PS5** and **Switch**, with features including:

- Stremio account sign-in with a link code, with your library, add-ons and watch progress synced through the official `stremio-core`
- Trailers that play automatically on the home screen as you move between titles (PS5)
- Home board, Discover, Library, Calendar, Add-ons and search with the console's on-screen keyboard
- Title pages with seasons, episodes and stream lists, for HTTP and debrid streams
- Player with seeking, scrubber previews, audio track selection, embedded and add-on subtitles with delay and styling, and resume
- An animated UI, with transitions between screens and optional sound effects
- 4K output and HDR-to-SDR tone mapping (PS5)
- Experimental subtitle auto-calibration, which times add-on subtitles to the dialogue using on-device speech recognition (PS5)
- A handheld UI that switches on automatically when undocked, and a stream quality limit (Switch)

PS4 and Wii U support is planned in the near future.

## Demonstration

<p align="center">
  <img src="docs/images/home.gif" alt="The home screen, with a trailer starting behind the featured title" width="49%">
  <img src="docs/images/episodes.gif" alt="A series' page: seasons, episodes and streams" width="49%">
</p>

<p align="center">
  <img src="docs/images/menu.gif" alt="Moving between the home screen, Discover, Library, Calendar, Add-ons and Settings" width="49%">
  <img src="docs/images/discover.png" alt="Discover, with a film chosen and its details beside the grid" width="49%">
</p>

<p align="center">
  <img src="docs/images/calendar.png" alt="The calendar of upcoming episodes" width="49%">
  <img src="docs/images/search.png" alt="Search results" width="49%">
</p>
