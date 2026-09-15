<p align="center">
  <img src="assets/diskdoc.jpeg" alt="Diskdoc dragon" width="350"/>
</p>

A simple and lightweight tool for checking the health of storage devices, based on the smartctl command. The tool provides a quick, easy-to-read command that's always available. 

If you're feeling lazy, *you can let the AI run the analysis for you*. 


<p align="center">
  <img src="assets/datadoc.png" alt="Diskdoc dragon" width="350"/>
</p>


## Requirements

- A computer with LinuxOS
- Root privileges

Very minimal :)

## Installation

### Quick install

You can install the program, using the installation script or you can compile it using gcc and make

```sh
./install.sh
```

Once installed, run it from anywhere with `sudo diskdoc`. If you want to use an AI model to perform the analysis, store your API key with `diskdoc --set-key <provider>`: it is written to `~/.config/diskdoc/credentials` with mode `0600`, under your own account and not system-wide.

### Manual build

```sh
make           
sudo make install   # copy it to /usr/local/bin/diskdoc
```

`PREFIX` defaults to `/usr/local` and can be overridden, e.g. `make install PREFIX=$HOME/.local`.

### Uninstall

```sh
./uninstall.sh            # keeps your API key
./uninstall.sh --purge    # deletes ~/.config/diskdoc/credentials too
```

The key is your own credential, not something the installer created, so it survives an uninstall unless you ask for `--purge`. `sudo make uninstall` removes just the binary.

## Usage

```
Usage: diskdoc [-h] [-a] [-q] [-i] [-d <device>]
       diskdoc -t <short|long> <device>
       diskdoc -k <openai|anthropic|gemini>

  -h, --help            show this help message and exit
  -a, --all             analyze every detected physical disk
  -q, --quiet           only print the summary line per disk, skip the detailed report
  -i, --ai              analyze a device with an AI model, using your stored API key
  -d, --device <name>   analyze a single device by kernel name (e.g. sda)
  -t, --test <mode>     start a short or long self-test on <device> (e.g. -t short nvme0)
  -k, --set-key <prov>  store the API key of a provider, read from the terminal

With no options, diskdoc scans the disks and lets you pick one interactively.
-t and -k cannot be combined with any other option.
```

**Since reading SMART data usually requires root, run diskdoc with `sudo`.**

### AI models

`-i/--ai` sends the smartctl report to a model and prints its answer. Store your key once:

```sh
diskdoc --set-key openai       # or anthropic, or gemini
```

The key is asked for on the terminal without echo and saved in `~/.config/diskdoc/credentials`, readable by you only. If you store more than one key, add a line `DISKDOC_PROVIDER=openai` to that same file to choose which one `-i` uses.

diskdoc looks for a key in this order and stops at the first one it finds:

1. the command in `DISKDOC_API_KEY_CMD`, if you prefer keeping the key in your own password manager (`export DISKDOC_API_KEY_CMD='pass show openai/api'`, together with `DISKDOC_PROVIDER`);
2. `~/.config/diskdoc/credentials`;
3. `OPENAI_API_KEY`, `ANTHROPIC_API_KEY` or `GEMINI_API_KEY` exported in your environment;
4. `/etc/diskdoc/.env`, deprecated: it is only read to tell you to move the key.

Sources 1 and 3 live in the environment, which `sudo` wipes: pass them through with `sudo -E` or use the credentials file, which always works.

Each provider uses a fixed default model:

| Provider  | Default model     |
|-----------|--------------------|
| OpenAI    | `gpt-4o-mini`      |
| Anthropic | `claude-sonnet-5`  |
| Gemini    | `gemini-1.5-flash` |

To use a different model, edit the corresponding `#define` in `include/dd_ai.h` (`OPENAI_DEFAULT_MODEL`, `ANTHROPIC_DEFAULT_MODEL`, `GEMINI_DEFAULT_MODEL`) and recompile with `make && sudo make install`.

### Exit codes

diskdoc exits with the severity of the worst finding across the analyzed disk(s), so it can be used in scripts and health checks:

| Code | Meaning                                   |
|------|--------------------------------------------|
| 0    | All checked values are OK                  |
| 1    | At least one value needs watching          |
| 2    | At least one value is in alarm             |
| 3    | diskdoc could not run (bad arguments, missing dependency, ...) |

---

### Credits

Special thanks to the authors of the libraries used in this project.

https://github.com/davegamble/cjson
