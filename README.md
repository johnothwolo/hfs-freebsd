# FreeBSD HFS Driver

This is a WIP port of Apple's HFS driver to FreeBSD. The code is based on version 556.60.1.

To build the driver, clone the repository, navigate to the directory, then run make. Keep in mind that:

* This is a read-only driver. Write support is disabled for now.
* `mount_hfs` is untested, so use the builtin `mount` command e.g. `mount -t hfs /dev/da0p3 /mnt/disk`.

# Helpful Links
* [https://wiki.freebsd.org/HFS](https://wiki.freebsd.org/HFS)
* [https://github.com/apple-oss-distributions/hfs](https://github.com/apple-oss-distributions/hfs)
* [https://developer.apple.com/library/archive/technotes/tn/tn1150.html](https://developer.apple.com/library/archive/technotes/tn/tn1150.html)
