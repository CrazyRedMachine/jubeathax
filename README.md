[![Donate](https://img.shields.io/badge/Donate-PayPal-green.svg)](https://www.paypal.com/donate?hosted_button_id=WT735CX4UMZ9U)

# jubeathax

Patcher for jubeat arcade game.

Basically a fork of [popnhax](https://github.com/CrazyRedMachine/popnhax) I made in order to interface a Launchpad MK2 (or its Mini variant)

Which means it's also based on [bemanihax](https://github.com/windyfairy/bemanihax)

## Main feature: External IO

This will hook the game at various places in order to interact with pretty much any kind of controller,
as long as you have a compatible IO dll for it.

IO dlls are dynamically loaded plugins so **you can write your own DLL without having to worry about how to hook the game**.

### Included plugins (Supported hardware)

#### io_dummy.dll

This is a plugin template which just prints status messages in your terminal.

Meant to be used as a reference on how to add a new plugin.

#### io_launchpad.dll

- Launchpad MK2
- Launchpad Mini MK2

#### FAQ: How to add support for my own controller?

If you can write C or C++ code which interacts with your controller then you can add support. It is just a matter of implementing the desired behavior
for the different status messages sent by the engine (cf. `io_plugin_board_update` function).

Refer to [cell_state.h](https://github.com/CrazyRedMachine/jubeathax/blob/main/jubeathax/cell_state.h) for more information about which messages
are sent by the engine and when.

Refer to [this commit](https://github.com/CrazyRedMachine/jubeathax/commit/bf43dcdf88be0a701c839dc42e6d321858c060f4) for more information about how to add a new plugin in the build system.

### Other Features

I also kept the legacy patches from bemanihax, refer to [jubeathax.xml](https://github.com/CrazyRedMachine/jubeathax/blob/main/dist/jubeathax/jubeathax.xml) for complete list.

### Run Instructions

- Extract all files directly in the `contents` folder of your install (you can either keep or remove the unneeded io plugin dlls, jubeathax won't mind).

- Edit `jubeathax.xml` with a text editor and set your desired options.

- Add `jubeathax.dll` as an inject dll to your gamestart command or option menu.

eg. modify your gamestart.bat to add `-k jubeathax.dll` or `-K jubeathax.dll` depending on the launcher you use.

Some launchers also feature an option menu (accessible by pressing F4 ingame), in which case you can locate the "Inject Hook" setting in option tab. Enter `jubeathax.dll` there.

### Build Instructions

Using WSL is the recommended method. Just run `make`.