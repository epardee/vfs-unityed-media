### Installation instructions ###

#### Requirements: ####
  * Samba 4.0.0 or higher, configured and running
  * tools for building from source, eg.

```
yum -y groupinstall "Development Tools"```
or
```
apt-get install build-essentials```

_To get version of Samba installed on your system, type_smbstatus_in terminal window._

#### Compiling ####
  * configure samba sources
- download and extract samba source tarball (corresponding with installed version) from samba.org, eg.
```
wget http://www.samba.org/samba/ftp/stable/samba-4.0.7.tar.gz```
- extract source archive
```
tar -xvf samba-4.0.7.tar.gz```
```
cd samba-4.0.7/source3```
- run ```
./autogen.sh```

- run configure ```
./configure```
  * compile Unityed Media
- download and extract Unityed Media source files from [source download area.](http://code.google.com/p/vfs-unityed-media/source/browse/um_downloads/source)

- in the directory with UM source files run
```
./autogen.sh```

- run configure in the following way: ```
./configure --with-samba-source=/path/to/samba/source3```

- and then ```
make```
_On FreeBSD you should use_gmake_instead_make_._

_Alternatively, look in [binaries](http://code.google.com/p/vfs-unityed-media/source/browse/um_downloads/binaries/) for compiled version for your system._
#### Installing ####

Your new compiled "unityed\_media.so" library will be installed in (or should be copied to):

1. /usr/lib64/samba/vfs if you build for an RPM-based Linux system

2. /usr/local/samba/lib/vfs if you build for an FHS-based Linux system.

It might also be installed elsewhere depending on your environment.

#### Share configuration ####

Add this module to the vfs objects option in your Samba share
configuration (probably /etc/samba/smb.conf).
eg.

```
...
[avidshare_win]
path = /video
vfs objects = unityed_media
```

If you add```
vfs objects = unityed_media``` to ```
[global]``` section of smb.conf, it's become default for all shares on your Samba server.

You can separate out Samba shares for Mac
and Windows clients, and add the following options to the shares
for Windows clients

```
veto files = /.DS_Store/._*/.Trash*/.Spotlight*/.hidden/.hotfiles*/.vol/
delete veto files = yes
```

This prevents hidden files from Mac clients interfering with Windows
clients. If you find any more problem hidden files then add them to
the list.

#### Avid settings ####


Start Avid, go to "Tools" / "Console" and enter ```
alldrives 1```
You should see confirmation message "All drives will be viewed as media drives".


To deactivate this, enter ```
alldrives 2``` and you should get "Only true media drives will be viewed as media drives".