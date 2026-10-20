k64_add_rom(NAME 22_music SOURCES main.c TITLE "K64 22 MUSIC"
            MUSIC song=song.mid
            SOUNDS "lead=lead.wav:--loop 1024" "bass=bass.wav:--loop 1024" kick=kick.wav snare=snare.wav hat=hat.wav
            FILES pad=pad.pcm)
