# Diskdoc

A simple and lightweight tool for checking the health of storage devices, based on the smartctl command. The tool provides a quick, easy-to-read command that's always available.

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

Once installed, run it from anywhere with `sudo diskdoc`.

### Manual build

```sh
make           
sudo make install   # copy it to /usr/local/bin/diskdoc
```

`PREFIX` defaults to `/usr/local` and can be overridden, e.g. `make install PREFIX=$HOME/.local`.

### Uninstall

```sh
sudo make uninstall
```

## Usage

```
Usage: diskdoc [-h] [-a] [-q] [-d <device>]

  -h, --help            show this help message and exit
  -a, --all             analyze every detected physical disk
  -q, --quiet           only print the summary line per disk, skip the detailed report
  -d, --device <name>   analyze a single device by kernel name (e.g. sda)

With no options, diskdoc scans the disks and lets you pick one interactively.
```

Since reading SMART data usually requires root, run diskdoc with `sudo`.

### Exit codes

diskdoc exits with the severity of the worst finding across the analyzed disk(s), so it can be used in scripts and health checks:

| Code | Meaning                                   |
|------|--------------------------------------------|
| 0    | All checked values are OK                  |
| 1    | At least one value needs watching          |
| 2    | At least one value is in alarm             |

---

### Credits

Special thanks to the authors of the libraries used in this project.

https://github.com/davegamble/cjson
