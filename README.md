# jAudioShim
Simple audio output for J. Works for J9.7, debian 13 with portaudio19-dev installed.

Plays floats in _1 1 range as sounds. Use interleaved data for multiple output channels.


1. Compile jaudioshim.c :
```shell
gcc -shared -fPIC -O2 -o libjaudioshim.so jaudioshim.c -lportaudio -lpthread
```

2. Call libjaudioshim.so functions with theses J bindings :
```J
LIB=:'myfolder/libjaudioshim.so'
audioTerminate=:{{0{::(LIB,' shim_terminate n') cd ''}}
audioInit=:{{(LIB,' shim_init i i i') cd y [ audioTerminate ''}} 
audioPlay=:{{0{::(LIB,' shim_play i *f i') cd y;(#y)}}
audioStop=:{{0{::(LIB,' shim_stop n') cd ''}}
audioStatus=:{{0{::(LIB,' shim_status i') cd ''}}
audioPause=:{{0{::(LIB,' shim_pause n') cd ''}}
audioResume=:{{0{::(LIB,' shim_resume n') cd ''}}
audioGain=:{{0{::''[ (LIB,' shim_set_gain n f') cd (,y)}}
audioPos=:{{0{::(LIB,' shim_position i') cd ''}}
audioSeek=:{{0{::(LIB,' shim_seek i i') cd (,y)}}
```
3. Micro documentation :
```J
audioTerminate''	NB. Empty argument.
audioInit y			NB. Receives SOUNDRATE (an integer, e.g. 16000, 32000, 44100...) and CHANNEL COUNT (1 for mono, 2 for stereo, or more). Use interleaved data for non-mono output.
audioPlay y			NB. Receives an array of floats in _1 1 range (the sound to play). Replaces any sound currently playing (short crossfade to avoid clicks).
audioStop''			NB. Empty argument
audioStatus''		NB. Returns 0=idle 1=fade-in 2=steady 3=crossfade 4=fade-out. Empty argument.
audioPause''		NB. Freezes playback, position preserved.
audioResume''		NB. Resumes from where it was paused. Empty argument.
audioGain y			NB. Receives a non-negative float. 1.0 = original volume, values above 1.0 amplify (risk of clipping).
audioPos''			NB. Returns current frame position, _1 if idle. Empty argument.
audioSeek y			NB. Receives the frame number to jump to (same unit as audioPos).
```
Each returns error number (0 for success), except audioInit which returns error number, sound rate and channel count.

Example :
```J
sr=:16000
osc=:1 o.2p1*1|[%~[:+/\#
audioInit sr;1  
audioPlay sr osc 440 
audioTerminate''
```
