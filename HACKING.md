Developer tips for GNOME Online Miners
======================================

To test a miner from the commandline, first run the miner in a terminal:

    ./src/gom-facebook-miner

Next, call the RefreshDB method using `gdbus`:

    gdbus call --session --dest org.gnome.OnlineMiners.Facebook --object-path /org/gnome/OnlineMiners/Facebook --method org.gnome.OnlineMiners.Miner.RefreshDB "['documents', 'photos']"

The logging domain for GNOME Online Miners is `Gom`, so you can enable
debugging output by setting this in the environment:

    G_MESSAGES_DEBUG=Gom

