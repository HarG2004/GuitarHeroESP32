# Guitar Hero Clone on an ESP32
This is a project that implements the features of the game guitar hero onto an esp32 with a few components including a speaker, amplifer, sd card, led matrix, lcd,
and a keyboard.

The user should be able to select a song by interacting with the keyboard and looking at the song selection on the lcd. Once a song is selected the user has to time hitting
keyboard keys along with the lights that show up at the bottom of the led matrix. By correctly timing hitting keyboard keys the user earns points that sum to a final score once
the song ends.

# Current Functionality
Currently only the lcd and speaker have been added to the project. The lcd can display the text I want using the lcd module I developed using the esp32 I2C library, no external
lcd library was used. The speaker can play songs stored on an sd card connected to the esp32 using sdmmc, and the song is sent by I2S to an amplifier connected to a speaker.
The amplifer works as digital to analog converter and amplifies the signal for the speaker.

# Problems and Resolutions
Problem: Previously the lcd would not work. The problem was that the lcd was not being properly initialized into 4-bit mode at startup.
Fix: Followed the instructions for initializing 4-bit mode in the lcd documentation.

Problem: The lcd was still not working. This was due to the enable bit not being transistioned from 1 to 0.
Fix: Sent 2 messages to the lcd with the same command but the first message contained enable = 1 and the second contained enable = 2.
