# jaudioshim
Simple audio output for J.
Works for J9.7, debian 13 with portaudio19-dev installed.


1.Compile audioshim.c :
```shell
gcc -shared -fPIC -O2 -o libaudioshim.so audioshim.c -lportaudio -lpthread
```

2. Call libaudioshim.so functions with theses J bindings :
```J
LIB=:'myfolder/libaudioshim002.so'
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

Example :
```J
sr=:16000
osc=:1 o.2p1*1|[%~[:+/\#
audioInit sr;1  
audioPlay sr osc 440
audioTerminate''
```
