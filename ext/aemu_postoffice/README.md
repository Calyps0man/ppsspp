### aemu_postoffice

PSP adhoc data forwarder protocol and implementation, for easy and reliable adhoc multiplayer through internet

Current users:
- [PSP internet adhoc plugin aemu](https://github.com/kethen/aemu)
- [PPSSPP](http://github.com/hrydgard/ppsspp)

#### Current design

See [design.md](/design.md)

#### Client implementation

See [./client/postoffice.c](/client/postoffice.c)

##### Building and testing

Linux:

```
# Ubuntu/Debian:
apt install podman git

# OpenSUSE
zypper install podman git

# Fedora
dnf install podman git

# Clone project and build client
git clone https://github.com/kethen/aemu_postoffice
cd aemu_postoffice/client
bash build_podman.sh

# Run tests, requires relay server on localhost running (see below)
./test.out
```

Windows:

1. install https://cygwin.com/ , pick packages `mingw64-x86_64-gcc`, `mingw64-x86_64-gcc-g++` and `git`
2. open a cygwin shell

```
# Clone project and build client
git clone https://github.com/kethen/aemu_postoffice
cd aemu_postoffice/client
bash build_windows.sh

# Run tests, requires relay server on localhost running (see below)
./test.exe
```

#### Server implementation

See [./server_njs/aemu_postoffice.ts](/server_njs/aemu_postoffice.ts)

##### Running server

See [./server_njs/usage.md](/server_njs/usage.md)
