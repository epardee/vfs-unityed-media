**Unityed Media** is a Samba VFS module that allows multiple Avid clients to share media.

Sucessfully tested with Windows7 64 bit/Media Composer 6.5 (clients) and Fedora18 64 bit/Samba 4.07 (server). Corresponding binary can be found [here](http://code.google.com/p/vfs-unityed-media/source/browse/um_downloads/binaries/linux-x64).

Source files can be downloaded [here](http://code.google.com/p/vfs-unityed-media/source/browse/um_downloads/source), please read [installation instructions](http://code.google.com/p/vfs-unityed-media/wiki/Installation) before you begin.

Unityed Media is related with [Media Harmony VFS](http://www.samba.org/samba/docs/man/manpages/vfs_media_harmony.8.html), so if you have a Samba4 installation on your server, try it! The main difference between Unityed Media and Media Harmony is that Unityed Media doesn't need manual refreshing of media directories. Unityed Media handles your media files in similar way as Unity, ISIS, EditShare or another dedicated solution does. Without client-side application and on hardware of your choice.

##### Some examples how it works: #####

| **Media directory seen by client "editor01"** | **Media directory on server** |
|:----------------------------------------------|:------------------------------|
|Avid MediaFiles/MXF/1                          |Avid MediaFiles/MXF/1\_editor01.1|
|Avid MediaFiles/MXF/7771                       |Avid MediaFiles/MXF/7771\_editor01.7771|
|Avid MediaFiles/MXF/1/15                       |Avid MediaFiles/MXF/1\_editor01.01/15|
|Avid MediaFiles/MXF/7MyDir12                   |Avid MediaFiles/MXF/7MyDir12   |
|Avid MediaFiles/MXF/1\_editor02.1              |Avid MediaFiles/MXF/1\_editor02.1|
|Avid MediaFiles/MXF/45\_editor02.45            |Avid MediaFiles/MXF/45\_editor02.45|
| **the same structure seen by client "editor02"** | **on server**                 |
|Avid MediaFiles/MXF/1\_editor01.1              |Avid MediaFiles/MXF/1\_editor01.1|
|Avid MediaFiles/MXF/7771\_editor01.7771        |Avid MediaFiles/MXF/7771\_editor01.7771|
|Avid MediaFiles/MXF/1\_editor01.01/15          |Avid MediaFiles/MXF/1\_editor01.01/15|
|Avid MediaFiles/MXF/7MyDir12                   |Avid MediaFiles/MXF/7MyDir12   |
|Avid MediaFiles/MXF/1                          |Avid MediaFiles/MXF/1\_editor02.1|
|Avid MediaFiles/MXF/45                         |Avid MediaFiles/MXF/45\_editor02.45|

Unityed Media does not support project sharing (yet).