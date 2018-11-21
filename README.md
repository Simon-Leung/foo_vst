Foobar2000 VST adapter
======================
VST 2.4 adapter is a component which aims to allow Foobar2000 users to use VST 2.4 plug-ins equally with “native” ones. Thousands of freeware and commercial DSP plug-ins are available in this format.

Usage
======================
First, add VST plug-ins you would like to use in the VST manager: ''Preferences → Components → VST plug-ins''. Restart is required for the changes to take effect. After the restart open the Foobar2000:Preferences:Playback:DSP Manager|DSP Manager or some conversion dialog box to try the plug-ins you've added.

Use ''View → DSP'' menu to access DSP configuration windows. Bind commands by their number to keyboard shortcuts if necessary.

VST program file (.fxp) import/export items are in the system menu of editor windows (right click on the window's titlebar).

Microsoft Visual C++ 2008 Redistributable Package (x86) may be required for versions below 0.8.1

Features
======================
The component has support for the following feature set:
* Multiple instances
* Multiple channels (including asymmetric configs like 2.0 → 5.1)
* Chain presets and secondary DSP chains (“convert” feature)
* Modeless DSP configuration windows with Foobar2000:Preferences:General:Keyboard Shortcuts|keyboard shortcut binding
* FXP import/export

Limitations and known issues
======================
* Limited to 20 VST entries due to API limitations (not to be confused with particular instances in a chain).
* No support for VSTs without custom editors yet (to be implemented in the near future).
* Display goes ahead of audio by the Foobar2000:Preferences:Output#Buffer_Length|size of the output buffer.
* No x64 VST support: Foobar2000 is a 32-bit application. However, most effect plug-ins are available as 32 bit binaries.
* Modeless DSP configuration functionality isn't well synchronized with the DSP manager in the Preferences window.
* There are two versions labeled as v1.0 and v1.0.01 which are actually v0.1 and v0.2.

Most of these problems are caused by diametrically opposite approaches to the management of DSP objects lifetime. In contrast to VST, Foobar2000 assumes plug-in's user interface to be highly decoupled from the DSP code and the most difficult task solved by the adapter is coupling things back. Actually, such kind of DSPs as analyzers in fb2k is even impossible due to lack of connection between the GUI and audio rountines (of course one can have analyzers in form of visualization, but it's out of processing chain then). Foobar2000 recreates DSP objects whenever settings or playback state is changed, while in VST plug-ins are supposed to be loaded only once and their settings are to be changed ''directly'' with either automation or a editor window. Also in VST, plug-ins rely on their host in respect of number of channels, but in Foobar2000 it's the other way round, i.e. the host renders as many channels as a plug-in has. Finally, VST host must provide a window for the plug-in to create a editor and it's not supposed to block windows underneath while Foobar2000 requires config windows to be modal, etc.

Given all this, it must be clear that something had to be sacrificed for the sake of features listed above in the article. The adapter keeps track of current DSP chain to ensure proper threading and gapless playback, it abuses programming language features to overcome “one DSP service per class” principle, it adds random data to presets and looks for a window title before opening a editor to distinguish different instances of the same VST effect, it has advanced settings to deal with some other problems as well. In many ways the adapter's reliability is limited by these tricks, yet there are many faulty VST plug-ins around.

Important notes
======================
* The adapter doesn't scan plug-ins at startup nor does it track changes in the VST directory. If some plug-in is absent then the adapter bypasses its processing.
* The component stores its settings in a separate binary file because of API limitations.

Settings
======================
This component has only advanced settings placed in the corresponding section of Foobar2000 preferences. Depending on the component version they can be found either in the Playback or VST category (the latter is true for v0.6+). All the problems that mentioned here are caused by the differences between Foobar2000 and VST approaches to DSP implementation.

__Declicker size (256*n samples, 4 by default)__  
&ensp;&ensp;Number of zero samples to be passed through VST on transport switches to suppress plug-in's output data that can produce click at the beginning of the next track, i.e. ringing of filters or delay effect tail. The latter requires this number to be increased. Changes to this setting take effect after restart.

__Limit number of outputs__  
&ensp;&ensp;In contrast to DAW, audio players don't manage outputs of their DSPs. Besides, many VST plug-ins don't report activity of their outputs. Hence the only way to output data from 16-channel VST is to drop some of the channels according to the setting.

__VST idle time to unload DLL (0-10,000 ms) (0.7.1 and below)__  
&ensp;&ensp;The delay is needed to suppress excessive unload-load cycles between tracks, i.e. to keep VST plug-ins loaded as it is supposed to be for them. In 0.8 the [un]loading strategy was revamped and this setting isn't necessary anymore.



Change log
======================  
__0.9.0.3__  
* Fixed removal on the player updates.  
__0.9__  
* Support for conversion and playback with same DSP chain presets.
* Preset manager for each VST.
* Per-instance channel number limit.
* DSP config shortcuts bugfixes.  
__0.8.1.0__  
* Revamped VST [un]loading strategy with regard to threading issues.
* Doesn't use registry anymore (hold Shift and press Add button to get VST entries back).  
__0.7.1__  
* Bugfixes in the portable mode support.  
__0.7.0__  
* Serious bugfixes.
* Relative paths in portable mode.  
__0.6.0__  
* Added FXP export/import.
* Improved View → DSP menu.
* Introduced declicker.  
__0.52__  
* Fixed the bug which could lead to crash after stopping the playback.  
__0.51__  
* Added experimental support for no-reload track switching.  
__0.5__  
* Added experimental support for non-modal config dialogs (View → DSP).
* Added some formal procedures for some capricious plug-ins to work.
* Added output number limitation setting.  
__0.2__  
* Fixed major stability issues. Marked as v1.00.01.  
__0.1__  
* Initial release marked as v1.0. Very unstable.
