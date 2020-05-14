GNOME Online Miners - crawls through your online content
========================================================

Bugs: https://gitlab.gnome.org/GNOME/gnome-online-miners/-/issues
Download: http://download.gnome.org/sources/gnome-online-miners
Git: https://gitlab.gnome.org/GNOME/gnome-online-miners/
Website: https://wiki.gnome.org/Projects/GnomeOnlineMiners

How to Use
----------

GNOME Online Miners is a "middleware" component: it's expected to be useful
to developers making applications, rather than being used by desktop users
directly.

The miners do not run in the background continuously. Instead, applications
interested in remote content need to use D-Bus to call the
org.gnome.OnlineMiners.Miner.RefreshDB method. You should call this regularly
while the application is running, in order to keep in sync with server-side
changes.

See the GNOME Documents and GNOME Photos applications for further examples of
how to use these miners.

License
-------

GNOME Online Miners is licensed under the GNU General Public License as
published by the Free Software Foundation, either version 2 of the License,
or (at your option) any later version.
