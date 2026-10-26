https://hackaday.io/project/170710/logs
ESP32 TNC and Audio Relay for HF/VHF Packet Radio
Back to overview

Wireless packet radio interface for HF/VHF/UHF transceivers, using ESP32 as KISS TNC or audio relay for use with soft modems

ryan-kinnettRyan Kinnett
Back to overview Files 0 Components 3 logs 10 Instructions0 Discussion 14
« Back to project detailsSort by: 
Newest

Newest
Looking for suggestions, collaborators
06/30/2020 at 21:27 • 0 comments
Hi all,

I rushed to get this working last week, ahead of Field Day, thinking I would use it to snag some digital contacts and bonus objectives, although I didn't end up using it.  Nevertheless, I'm thrilled that it's working and I have tons of ideas about where to go from here.  

In the near-term, I'll focus on getting it working more reliably with Winlink to support ARES practice operations.  I also plan to use this in conjunction with a satellite tracker project (writeup coming soon..) to bounce APRS messages off ISS and other packet relays sats.

I'm thinking that I'll rewrite the title and description of this project to make this purely an audio relay for PC-based modem software.  That is, unless any followers here want to advocate for onboard modulation/demodulation as I had originally planned?  I've already done most of the hard parts for this, just need to carry it across the finish line, but I'm losing steam.

I haven't yet built up a self-served html interface page.  I'll probably do that in coming weeks and will report back when it's done.  As part of this, I want to set up wifi credential handling to be similar to many commercial IOT devices, where it defaults to beaconing its own AP which you can log into and use the html configuration page to direct it to your home wifi.

After that, I'll probably slow down my time investments in this project unless I can drum up more interest to keep me motivated.

So, anyone out there interested in working with me on this project?
I could use casual suggestions, testers, collaborators, anything.

Would anyone want to try building your own if I write up instructions?

I have to admit, my code is a bit amateurish, cobbled together from many disparate pieces, and the organization is haphazard.  It could use a critical eye from legit programmers if anyone wants to dig into it.

I'm also thinking that I'll just wire up the audio bias/level circuits and PTT circuit point-to-point on perf board unless I can get someone to help design a legit circuit board as a backpack for ESP32 devkit boards with headers.  I also need to work out power interfaces, either a LiPo interface or a buck/boost converter to abstract power from the radio.  This project currently runs off USB port power.

Another intriguing idea (which I likely won't get to anytime soon unless someone wants to help with it) is making this work as an APRS iGate.  This would require finishing development of onboard AFSK demodulation, plus AX.25 packet handling, but once that's in place, forwarding captured messages to aprs.fi would be a breeze.

Please drop a line if you have ideas or are interested in testing and/or collaborating.

Thanks

73

It works! 2-Way Network Audio Relay
06/24/2020 at 10:00 • 0 comments
I'm happy to report that the 2-way network audio interface branch of this project is fully operational!

I have to admit, I dove into this quite naively, expecting it to be trivial to splice my audio-in and audio-out demos together with some simple logic to handle transitions between transmitting and receiving modes.  That mode switching logic did fall into place quite easily, but there were many other issues that made this an uphill battle.

First, when I embarked on developing this audio relay method, I was planning to take advantage of both of the ESP32's I2S engines, assigning one to audio input and the other to audio output.  Turns out the onboard DAC and ADCs are only accessible through the base I2S engine, I2S0.  The fun thing is, if you try to assign I2S1 to handle the built-in DAC, the ESP32 crashes, with useful debugging info in the backtrace, whereas if you assign I2S1 to handle the built-in ADC, it doesn't crash, it just silently doesn't work.  This set me a back several days.  After finally realizing that both streams would need to go through I2S0, it took another several more days to work out the right sequence of function calls (and timing!) to reliably hot swap the I2S0 interface between the DAC and the ADC.

The need to thread both audio-in and audio-out through a single I2S engine forced me to confront mode switching sooner than I had planned, but on the plus side, that turned out to be quite easy to do.  In a nutshell, the switching logic is embedded in the UDP-receive loop;  it transitions to transmitting mode when receiving non-zero data from the PC, otherwise falls back to receiving mode.  In pseudocode:

For each iteration of the UDP-receive loop,

    If UDP received && valid VBAN packet && contains non-zero data,
        reset no-data counter
        if mode==RECEIVING,
            transition to TRANSMITTING
            send [N] zeroes to DAC to transmit quiescent tx-delay
        send packet to DAC

    Otherwise (no UDP or not VBAN or packet contains all zeroes),
        increment no-data counter
        if mode==TRANSMITTING && no-data counter >= persistence,
            transition to RECEIVING
In the above pseudocode, each of the "transition to" lines is a function call to a mode transition handler which basically shuts down i2s, clears the DMA buffer, triggers or releases PTT depending on the commanded new mode, and restarts i2s with either the DAC driver for transmission or the ADC driver for receiving.  Note that, immediately after transitioning to TRANSMITTING mode, I send several DMA  buffer widths of zeroes to I2S to have it stream quiescence for a brief period after triggering PTT and before writing packet data to analog-out.  This is basically a crude, hardcoded-for-now implementation of what's commonly referred to as "tx-delay".  I'll parameterize this later and make it configurable via html interface.

Another problem I ran into was configuring the DMA buffer lengths.  In the outgoing audio stream (PC->ESP32), VBAN automatically selects packet widths between 64 samples per packet when the sampling rate is 11025Hz, and 256 samples per packet when sampling 44100 Hz or faster.  So VBAN sampling rates of 11025, 22050, and 44100 each produce power-of-two samples per packet, whereas even-thousands sampling rates (i.g. 12000, 24000, 32000) generate packets with non-power-of-two samples per packet.  Why does this matter?  It seems I2S works best with power-of-two DMA buffer widths, otherwise it seems the ADC stutters.  So if I set VBAN sampling rate to 12000 Hz, VBAN sends 70 or so (I don't recall precisely) samples per packet, and if I pipe those directly into DMA without interim buffering then I2S stutters.  It's possible that this apparent pattern is due to something I misconfigured during my experimentation, but I basically concluded that this should only be used with VBAN sampling rates of 11025, 22050, 44100 Hz, or higher (all sample rates above 44100 produce 256 samples per packet).

The working code is posted here:
https://github.com/rkinnett/ESP32-2-Way-Audio-Relay

Here's the schematic for the audio ins-and-outs and PTT:


Not much to it.  Note that the received-audio bias trim pot should be manually adjusted such that the quiescent voltage on pin 33 is about 1.65V, the middle of the ADC's ~3.3V range.  Alternatively, this pot can be replaced by a voltage divider with the upper resistor resistance value roughly 2x that of the lower resistor.  I arbitrarily trimmed the Mic-in bias pot to roughly 50% and that seemed to work.  Using a second radio to listen to transmissions from the ESP32-UV5R pair, the audio level sounded just fine, not too quiet and not over-deviating.  I did not use a coupling capacitor on Mic-In to take out the inherent 1.65V DC bias from the ESP32 DAC, and I'm not sure why that works unless the UV-5R has an internal DC-blocking cap.

On the PC side, I'm using VBAN Banana (more on that later..) to receive and send audio over Wifi to and from the ESP32, and VBAN Banana routes the incoming and outgoing streams to and from soundmodem which handles the AFSK packet modulation/demodulation.  I then connect either Winlink Express or PinPoint APRS to soundmodem.

After dialing in the biases and working out some kinks in PTT control, I finally tried reaching the nearest Winlink gateway which is ~10 miles away with hills in the way - not trivial for a 5W HT, but doable.  It took a few hours of fiddling, but eventually I got a response!  I kept working it and tweaking things until Winlink Express was able to maintain a link, then I sent a position report and was thrilled to see my position marker show up on the Winlink map.  Success!  That was one successful message transfer out of dozens of attempts, so this still has a long way to go.  

Next, I moved my setup outside so I could point my Arrow-II Yagi toward the Winlink gateway, but discovered a new problem:  the UDP streams became far too unreliable after moving 30' away from my router.  That's slightly disappointing, but not really a surprise, I guess.  That lead me down another rabbit hole, setting up the ESP32 as an access point.  That by itself isn't difficult, but for the life of me I could not get 2-way UDP streaming working when I connected my laptop to the ESP32 AP.  After wasting a couple of hours on this, I eventually discovered an isolated note in a forum somewhere, saying that VBAN Voicemeeter doesn't play well with non-router access points.  Didn't see that coming!  Fortunately, it was an easy fix.  VBAN Banana doesn't have this problem.  

It was starting to feel like forces were conspiring against me on this project..

Anyways, after getting things working again with the ESP32 as its own AP, I set Winlink aside and started playing with PinPoint APRS through soundmodem, and got that working fairly quickly.  That is fun!  I'm easily picking up other people's APRS beacons from 30 miles away, and even hundreds of miles away with a digipeater in between.  My own beacons are reliably picked up by an iGate about 9 miles away.

So, in summary, 2-way network audio is working semi-reliably and playing nicely with APRS, both receiving and transmitting.  I have at least one existence-proof data point with Winlink but I think that will work reliably too when I get back to it, using a beam antenna.  I might try this during an upcoming Winlink practice session with my local ARES group.

I'm also very much looking forward to bouncing some APRS messages off satellites!

I'm planning to write a separate log to recap progress and work-to-go.  I'm thinking about stopping here, just using the ESP32 as a network audio relay, without finishing developing onboard modulation and demodulation.

Streaming Audio from PC to ESP32 Analog-Out
05/27/2020 at 10:13 • 0 comments
Short update on a significant breakthrough:  I worked out a reasonably reliable outgoing audio chain, using VBAN to send audio data packets over WiFi to the ESP32.  The ESP32 unpacks the audio stream and funnels the data into direct memory access buffers which i2s then pipes to an analog-out pin.  Ultimately this will serve as the conduit for sending packet data from SoundModem, running on the PC, over to the ESP32 and ultimately into the mic port of the VHF/UHF transceiver.

Demo code here:  https://github.com/rkinnett/ESP32-VBAN-Network-Audio-Player

That code could be useful for many things beyond my intended purpose.  An intercom system, for example, or an audio endpoint for streaming music or anything you want from your PC.  How about a drone-carried PA?  The ESP32's built-in DAC is only 8 bits, not good enough to serve as a network audio receiver for a legit HiFi stereo system, but I think I'll build one anyway, maybe using an off-the-shelf ESP32 audio dev board with higher resolution DACs and linear amplifiers.  That should integrate very easily with this method since I'm already using i2s.

I previously worked out audio sampling and streaming from ESP32 to PC to relay audio received from the radio to SoundModem for demodulation.  With these two pieces in place, I'm now ready to design a state machine to juggle transmit and receive modes.  The high-level switching logic should be simple, but I'm not sure how easy it will be to dynamically change i2s input/output modes on the fly..

Streaming Received Audio over Wifi/UDP
05/18/2020 at 03:36 • 0 comments
It’s been a few weeks since my last post, but I have  made steady progress on several fronts.  I have focused mostly on the hardest part of this project:  real-time onboard FSK demodulation.  

I explored two approaches for audio sampling:  one using I2S and DMA, similar to the fast audio streaming demo I described in a previous log, and another using a timer + ISR approach as described by Ivan Voras here.  I will write more about these experiments later, in an update to this log, or in a separate one.  I was able to get both working 90% smoothly, but the streams would drop out every few seconds, for a fraction of a second.  After days of trying to optimize the code and clean up these dropouts, I haven’t yet gotten to the bottom of it.  I had Arduino OTA enabled and was printing to serial, doing some non-trivial batch sample processing, and streaming data over web sockets, all at the same time.  I have not yet tried isolating these interfaces to identify the bottleneck.  Will do that soon.

Despite the intermittent dropouts, I did have some success testing a tone identification method which will likely serve as the core of my onboard demodulation process.  I implemented the dual sine filter algorithm described in a late edit to my earlier log regarding demodulation techniques.  I set up a websocket to stream the isolated 1200 Hz and 2200 Hz magnitude values along with raw audio, and tested with a tone generator. It worked beautifully!  I’ll post more about this after I work out the periodic dropouts issue.

What’s next with development of FSK demodulation?  Getting the 1200/2200 Hz discrimination algorithm working was a big step.  After I get to the bottom of the dropouts problem, the next lesser challenges will be 1) implementing some sort of normalization and thresholding process, 2) first-frame identification, and 3) automatic sample phase optimization/synching.

Onboard demodulation is coming along, but still has a long way to go to make it reliable.

The more exciting news for this log is that I got half of the 2-way audio relay mode working.  The ESP samples analog audio, buffers and streams over wifi, via UDP and the VBAN protocol.  A VB-Audio Virtual Cable running on the PC receives the audio stream and presents as a soundcard driver.  I am very impressed by the VBAN utilities, which are fully functional donationware, and will happily endorse that project as I get this one off the ground.  It seems to be working smoothly.  I don’t hear noticeable dropouts when I sample general audio through the ESP32, although VBAN does report some packet losses which I have not yet investigated.  I will post my Arduino sketch of this test soon.  Should be handy for intercom systems.

I initially tested this received-audio sampling and streaming method by playing Stephen Smith (WA8LMF)’s APRS audio sample CD as analog input into the ESP32, streaming the digitized audio over UDP to VBAN Voicemeeter, then decoding the audio stream using UZ7HO’s soundmodem.  This setup reliably decoded messages from the test CD.  I have not systematically characterized reliability by counting total decoded packets for each track, which seems to be a common benchmarking practice.  I later hooked up my Baofeng UV-5R audio-out to the ESP32 and successfully decoded many live APRS transmissions.  This is huge!

UPDATE: 

I posted demo code here:  https://github.com/rkinnett/ESP32-VBAN-Audio-Source

One thing worth noting, I originally planned to have the ESP32 broadcast audio without specifying a destination address, but found this approach to be choppy and intermittent, completely unusable.  After some googling, I found that UDP broadcast and multicast transmission modes are not generally recommended for even moderate data volume applications.  I will need to work out how to have the ESP32 dynamically configure the destination address.  Initial thought is that I'll just include an IP address field in a configuration webpage which the ESP32 will host after I build a few more of the core pieces of this project.

Also worth noting, I'm not seeing occasional dropouts like I observed while streaming over WebSockets.  The UDP stream is continuous, and Voicemeeter very rarely indicates packet errors.  I'm thrilled to watch soundmodem decoding the majority of packet transmissions that I can audibly hear.

Here's the demo!  


Oooh Shiny Object! (fast data visualization)
04/27/2020 at 01:24 • 0 comments
While reading about analog-digital conversion (ADC) on the ESP32, I found many references and examples using I2S to sample a GPIO pin at a configurable rate.  Most notably, github user zekageri had posted several questions in the esp32.com forums about glitches he was seeing in his streaming ADC data, and he shared this video of his ESP32-served web interface plotting data samples at 44.1kHz.  What is this magic?  WebSockets!  A relatively new and highly streamlined TCP-based protocol, facilitating fast and efficient data transfer.  Zekageri posted an early version of his code on github, but I couldn't get it to work out of the box.  I basically started from scratch, using some of the general concepts from his project, and evolved it considerably.

Fast streaming data visualization
This fast-streaming data visualization will be key to tuning my demodulation code which I will start testing imminently.  This data vis approach will also be a great tool to add to the tool box for fine tuning during operation later, i.g. to make sure incoming and outgoing audio streams are properly scaled and biased and not clipping.

Test/Dev Process
I threw the ESP32 on a breadboard and wired up a bias circuit by coupling an audio cable to a potentiometer as a voltage divider, pulling the signal up to about midway between ground and the ESP32's 3.3V regulated pin so that the ADC can ingest the full signal without clipping the lower half.  Here's the basic audio-in bias circuit:


I added that 8 Ohm speaker on the right channel for fun.  I might incorporate a speaker into the final design later, with a proper amplifier.

Next, I plugged the audio cable into the speaker jack on my laptop and used this handy online tone generator to send audio signal to the ESP32.  Before plugging in the ESP32, I tested the signal level with my oscilloscope and verified it was within the 0-3.3V measurement range of the ESP32 ADC.


Here's the data visualization code:
https://github.com/rkinnett/ESP32-ADC-Plotter-Demo

Basically, the ESP32 buffers many samples in DMA, via I2S, and the main loop periodically pulls that buffer and sends it over WebSocket as binary data.  The ESP32 serves a webpage with a CanvasJS plotting method to dynamically plot incoming data.

It took a lot of work to understand an inversion issue and that the ESP32 puts random (okay maybe not random; I'm not sure what it is) ones and zeroes in the upper 4 bits of each 16 bit sample.  The ADC measures in 12 bits, and I assumed the ESP32 would pack those 12-bit measurements into 16-bit values with zeroes in the upper bits, but I was wrong and that took a few frustrating hours to figure out.  Anyways, that's fixed, and now I know how to sample audio efficiently.

Check out the demo:


This should be useful for so many projects..

I hope you found this useful.  If so, please leave a comment!  Thanks, 73

Researching FSK demodulation methods
04/25/2020 at 10:48 • 2 comments
DISCLAIMER:  
This post summarizes my piecemeal, spotty, experimental, and in some cases probably incorrect understanding of the methods described herein.  This was all new to me before this project, and it still is, so don't take anything here as gospel.

About Frequency-Shift Keying (FSK)
Key characteristics of FSK are two discrete audio tones representing "marks" and "dashes", aka binary ones and zeroes, with smooth transitions between tones.  FSK is a non-returning-to-zero (NRZ) type encoding method, meaning each bit is explicitly represented by one frequency or another, as opposed to representing zeroes as lack of signal.  300-baud FSK (aka Bell 103 standard) is common on HF (SSB).  In this mode, an audio signal is composed of a continuous sine wave in which the frequency varies smoothly between two arbitrary frequencies, separated by 200 Hz, representing a stream of ones and zeroes at 300 symbols per second.  The modem injects this audio stream into a mic port of an HF transmitter which mixes the pre-modulated AF signal and transmits as SSB.  1200 bd FSK is more common on VHF (FM).  1200 bd packet uses 1200 Hz and 2200 Hz tones representing ones and zeroes, respectively, per the Bell 202 standard.  Note that this peculiar selection of tones avoids aliasing but also slightly complicates the process of digital demodulation.  Also note that the baud rate matches the frequency of the lower tone, meaning that just one single cycle of 1200 Hz tone fits within each symbol period - this turns out to be a driving factor in which tone differentiation methods can be used.

9600 baud is also possible, but requires a compatible radio which gives the modem direct control of the RF modulator, and even higher symbol rate modes are built on top of that.  

This project will initially focus on 1200 FSK since I currently only have access to VHF/UHF radios without modulator ports.

By the way, for context, FSK is one of several physical-layer modulation techniques used to encode digital data into audio frequency and ultimately RF.  The content of that digital data is coordinated between the link and application layers.  My primary use case for this project is to support Winlink for emergency comms, using the ESP32 either as a KISS TNC or as an audio relay.  In both of these modes, link layer coordination is handled by software on the PC and there is no need for the modem to implement higher-order protocols (AX.25).  However, I am also interested in supporting APRS, which probably will require onboard link control and AX.25 packetization.  In any case, my main point here is to explain the origin of the digital data that's going through FSK modulation and demodulation.  We will deal with link control infrastructure and packetization later.

----

Pulling bits out of the air
There are several ways to approach FSK demodulation, each with different levels of capability and resource requirements.  

The Fast Fourier Transform (FFT) is more or less the go-to method for differentiating between frequency components in a digitized analog signal for many, miscellaneous applications throughout engineering.  That said, the FFT is not particularly well suited for embedded FSK demodulation.  The FFT calculates energy in discrete frequency bins between zero and Fs/2, and these calculations are intertwined and inseparable.  Since we're only interested in two specific frequencies, most of the information generated by the FFT would be thrown away.  Further, FFTs are typically evaluated in adjacent, non-overlapping sample windows, with no information carried over from one window to the next.  Additional layers of complexity could work around these issues but generally would not be practical on an embedded microprocessor, even on one as capable as the ESP32.  

The next step closer is the Goertzel Algorithm which is more ideal for calculating energy in specific frequencies.  This method requires complex number math which is well supported in C and on the ESP32.  The method itself appears to be fairly simple, but for whatever reason, I couldn't seem to get it to produce meaningful signal in numerical simulation, after days of trying.  I'm sure it's just one little detail I missed.   Update:  I was able to get this method working, but I'm finding that it's severely constrained by the short window that I have to use in order to discern changes at 1200 baud.  Basically, at 26400 Hz sampling frequency, just 22 samples span each 1200 Hz cycle (the symbol rate at 1200 baud).  The frequency resolution of this method is dF = fs/N = 26400/22 = 1200 Hz, which is terrible for this application, given that I'm trying to differentiate between tones that are separated by only 1000 Hz.  I'm basically locked into this property because I can't extend the Goetzel evaluation window wider than one 1200 baud symbol period.  I can't just double sampling frequency because then I would have twice as many samples per 1200 baud period, resulting in the same ratio and same frequency resolution.  This interpretation is born out by experimentation described below.

I also experimented with a related approach, the Sliding Discrete Fourier Transform.  Similar to the FFT, the Sliding DFT transforms time-domain series of measurements into the frequency domain, yielding an energy spectrum across a range of discrete frequency bins.  The Sliding DFT is significantly less computationally complex than the FFT, and it also has an advantage of evaluating with each sample rather than in non-overlapping windows, reducing latency and improving efficiency by reusing previous calculations.  I tried several times to code it myself in various forms, then tried and found success with this SlidingDFT library by Bronson Philippa.  Here is a test I produced in C, using the SlidingDFT against simulated ADC data representing a random series of 31 bits, overlaid with gaussian noise, with SNR 2:1.  I chose a sample frequency of 26400 Hz which is an even multiple of both 1200 Hz and 2200 Hz, and a window of 22 samples which corresponds to one 1200 bd symbol period.  This method provided good discrimination between 1200 Hz and "not 1200 Hz", but the 2200 Hz harmonic was indiscernible.  The magnitude plot shows a reasonably smooth signal, with no additional low pass filtering needed, and the signal is sharp-edged enough to pick up solitary ones and zeros.  This is very promising!

After flailing a bit with the Goertzel algorithm, and before finding success using Philippa's SlidingDFT library, I had just about given up on these canonical methods and had worked out another numerical approach that also seems robust and less computationally complex.  I haven't found any documentation of a similar method, but I wouldn't claim to have invented anything here.  (EDIT:  this method is called a comb filter and is similar to a time delay correlator).  The general approach is as follows:  for each new sample x[n], subtract the sample that came 180 deg ahead of the current sample if the current tone is 1200 Hz, and take the absolute value of that difference.  With 26400 Hz sampling rate, the period of each 1200 Hz cycle is 22 Hz, so for this step, subtract the sample received 11 samples ago.  This makes a rectified 1200 Hz "resonator" (my terminology), call it "R1200".  Average across the last 1200Hz half-period (11 resonator values in this case) to smooth this out, and call this "S1200".  Do the same for 2200 Hz tone.  Calculate "envelope" as sum of S1200 and S2200.  Calculate differential "Diff" as difference between S1200 and S2200; this will be used with a threshold to differentiate between 1200 and 2200 Hz tones.

For each new sample x[n]:
R1200[n] = abs(x[n] - x[n-11])
S1200[n] = average(R1200[n-11:n])
R2200[n] = abs(x[n] - x[n-6])
S2200[n] = average(R2200[n-3:n])
Env[n] = S1200[n] + S2200[n]
Diff[n] = S1200[n] - S2200[n]
This approach which I stumbled upon experimentally looks very promising, but it's touchy.  There are a lot of knobs to turn, each with its own advantages and disadvantages.  More filtering, for example, cleans up noise-induced transients, but it also flattens out legitimate peaks which makes the method more prone to synchronization issues.  It's not guaranteed that I'll find an appropriate balance between these competing effects.  UPDATE:  I found this method to be susceptible to noise because each correlator step evaluates just 2 source samples, whereas some methods look for correlation in all samples within a moving window.  Further, this method is sensitive to differences in amplitude between the 1200 Hz and 2200 Hz segments, which is expected to be a real problem with some radios due to audio frequency emphasis/deemphasis filtering.

Simulation
I built a somewhat high fidelity numerical simulation in google sheets, of all places.  It simulates a brief period of background noise, followed by a stream of 31 random ones and zeroes which are FSK modulated, then I add random noise, some clipping and scaling to mimic the ESP32 ADC range, then the demodulation steps.  The signal and noise are scalable, and I made macros to resample the random bit patterns and signal noise so that I could test the demodulation methods against hundreds of combinations of signal and noise values and relative amplitudes.

Here is a snapshot from just one run:


The top plot shows the random bit pattern in blue, with the (eventually) decoded bit pattern (via time delay correlator method illustrated in third plot) in red.
The second plot shows the FSK modulated signal with simulated noise (SNR 2:1).
The third plot shows the 1200 Hz and 2200 Hz resonators, smoothed, the difference between them, and latched values from that differential, used directly for comparison to a threshold (near zero) to interpret decoded bits.
The fourth plot shows results from the SlidingDFT library.  The 2200 Hz trace is virtually unusable, but the 1200 Hz trace shows very clear, usable signal.  The "sdst pwr" trace shows the 0th DFT entry which might also be useful for enveloping and discrimination.
Update:

I also experimented with the Goertzel algorithm across the same simulation data.  This method produced a signal that roughly correlates with the 1200 Hz periods of the source bit pattern (the 1's), but it also oscillates during the 2200 Hz periods (the 0's), with peaks well past where I would want to set a threshold for the 1200 Hz signals.  I believe this is an aliasing effect, due to the fact that 2200 Hz is "not far" from the second harmonic of 1200 Hz, considering the 1200 Hz frequency resolution of this method.  I could conceivably take advantage of that oscillation by multiplying the signal by a copy of itself, phase lagged by half of a 2200 Hz period.  This would leave the steady plateaus correlating to 1200 Hz and would attenuate the oscillations corresponding to 2200 Hz, and the smoothing this would introduce would only stretch about 1/4 (6/22 to be precise) of the baud period.  For this simulation, I cut the noise down to SNR 4:1 and applied a 3-sample averaging filter to the raw samples before calculating Goertzel magnitudes.  I don't know, this might work, but it would take some additional creative post-processing to make this usable.


Another Update:
Reading further, I came across a nice writeup by Bernard Debbasch about how he implemented FSK demodulation as part of an ARM-based answering machine project.  His approach passes the audio signal through what, in concept, could be considered to be tuned oscillators, implemented as sets of sine and cosine values at each of the pattern frequencies, 1200 Hz and 2200 Hz.  He samples at 9600 Hz, providing 8 samples across each 1200 bd cycle.  At each 9600 Hz time step he convolves the latest 8 samples with the sine and cosine series and sums these products.  The cosine series represent the imaginary component of the signal and resonates when the source signal phase angle is near 90 and 270 deg.  The sine series represents the real component of the signal and resonates with either the 1200 Hz series or 2200 Hz series when the phase is near 0 or 180 deg.  RSSing the real and imaginary components yields the magnitude of the signal.  This approach produces a clear and relatively stable correlation with each filter.  Subtracting the outputs of the two filters makes it very easy to differentiate between 1200 Hz and 2200 Hz segments of the source signal.

(Fs=9600Hz, SNR=2)


In the bottom chart, I overlaid a windowed average of the 1200 Hz magnitude minus 2200 Hz magnitude difference signal to smooth it out, but that's not really necessary.  One can simply check the sign of the unfiltered difference signal.  This approach works very well at 9600 Hz.  There are several other references to this relatively simple and seemingly robust method, implemented on various older micro-processors.  The ESP32 would eat this for breakfast.

Okay one more update then I'm done
Shout out to fellow CalPoly alum Robert Campbell.  I ran across his thesis, "Analysis of Various Algorithmic approaches to Software-Based 1200 Baud Audio Frequency Shift Keying Demodulation for APRS" long after I dove into this exploration of FSK demod methods.  It was useful to read his analysis and comparison and roughly validate some of my own findings.

Conclusion:  
The Sliding DFT, time delay correlator, and tuned filter methods are very promising.  Need to try each on hardware and see which is most reliable.  With the time delay correlator method, I'm finding bit error rates on the order of 3%.  I'm not sure if that's good enough.  Update:  the Goertzel method also appears to be viable, but seems to produce a more chaotic signal and is relatively complex and does not specifically target my frequencies of interest.  I'll keep it mind, but I'm more inclined to start with the other three methods before this one.

Next step:  start experimenting with FSK demodulation on the ESP32

Assignable KISS interface!
04/11/2020 at 09:17 • 0 comments
I wrote my own KISS library, and I framed it such that any arduino Stream object may be assigned to the KISS and logging interfaces.  This will help later when I develop an html configuration page and need to change interfaces on the fly.

I posted a demo here:  https://github.com/rkinnett/Esp32-Bluetooth-KISS-Demo

In this demo, I associate the KISS interface with Bluetooth serial, and I assign logging to the (USB) serial port.  I'm using Winlink to generate KISS messages over Bluetooth and am showing the message parsing in the Arduino Serial Monitor window:


This demo is one-way only, from host to TNC. I have not yet worked out functions for encapsulating received AX.25 packets into KISS frames, and more generally, I have not worked out AFSK modulation and demodulation at all.

I expect AFSK modulation will be a piece of cake since I have already worked out the method to extract to-be-transmitted AX.25 packets from KISS frames, with KISS special character handling, so now it's just a matter of generating AFSK from the binary packets.  I don't need to know anything about the AX.25 protocol since the KISS interface is just the transport for AX.25 packets which are to be transmitted verbatim, except for special character handling.  Demodulation will be more interesting.  I'm hoping digitization of the frequency-modulated audio signal is straightforward, although others have reported problems with ADC timing on the Esp32.  I have not yet researched DSP methods for frequency-ID to convert the digitized frequency-modulated audio signal into binary and ultimately reassemble the received AX.25 packet.  Also, is error checking handled at the physical layer or at the link layer?  Encapsulating received AX.25 packets into KISS frames should be easy.

Next step:  AFSK modulation

Incremental Progress
04/07/2020 at 01:39 • 0 comments
I made a little progress over the weekend.

First, I rigged up my ID-5100a with an RJ45 splitter between the mic and the radio base so I could tap into the 8Vdc, PTT, and mic audio pins to observe these signals during normal operation.  The 8Vdc and PTT signals made sense, but I'm a bit baffled by the mic "audio" signal.  I expected to see an obvious "line level" audio signal, proportional in frequency and amplitude to the volume of my voice, but I saw nothing of the sort.  Instead, the voltage between the Mic Input and Mic Ground pins was basically 8Vdc plus a 300 mV pk-pk signal in the 2 MHz range.  What the heck?  Zooming in it does not look like some sort of digitized serial stream.  I scoped the other wires but couldn't find anything resembling an AF signal while I was talking into the keyed mic (and also hearing my own transmission, received on an HT).  What gives?  Still kinda baffled by this. 

Anyways, I see that Signalink jumpers the ID-5100a Mic Ground to common ground with PTT ground, and wires the Signalink MIC (output) pin directly to the ID-5100a Mic Input line.  I can't find an explicit description, I presume the Signalink just sends line level AF through that connection.  I might ask the ID-5100a facebook group about this since several guys in that group use Signalink with this radio.

----

I found several Arduino KISS/AX.25/APRS examples online, but none were implemented in a way that was intuitive to me, so I started down the path of defining my own KISS library and AFSK mod/demod elements.  I'll borrow bits and pieces from other projects to get forward error correction and other functions working quickly.

Starting from scratch, it turns out SerialBT is super easy to work with, and persistent, so I can reflash and test incrementally without my PC dropping the BLE association.  I easily set up Winlink in Packet mode through KISS interface, and pointed that to the serial com port of the ESP32 BLE receive channel, received KISS messages on the ESP32 and echoed them back to the USB serial interface.  The KISS TNC control messages immediately made sense but I haven't yet worked out the first data message which I presume is some sort of AX.25 CQ message.

Screenshots later..

Practicality Issues
04/05/2020 at 00:10 • 0 comments
Bidirectional audio streams?
Not sure how to implement bidirectional audio over BT with ESP32's single BT radio.  ESP32 can be configured as an A2DP source or sink, but not both simultaneously, apparently.  What I originally had mind was that the ESP32 would present itself as a BT headset, such that soundcard modulation software (i.g. soundmodem, FLdigi, etc) running on the terminal PC can select the ESP32 as both the input and output audio device, and use either as needed.  Since bidirectional A2DP does not appear to be a thing, in order to make this BT audio relay concept work, I would need to either find a way to dynamically switch between source and sink modes, or attach a peripheral BT transceiver and maintain parallel BT A2DP source and sink links.

I don't know if switching BT modes dynamically will break the Windows audio device targets, or if there is some way to maintain both links even while the ESP32 is not listening on one or the other.  If I can find a way to switch between source and sink modes without breaking the Windows audio targets, then the next step would be to figure out how to trigger mode switching.  The Winlink packet mode settings allow you to specify a com port for PTT, and that com port can be set to a virtual com port routing over TCP.  That should be feasible, albeit convoluted.  This makes the other idea, attaching a second BT radio to the ESP32, seem more appealing, but I was hoping to keep the parts list and cost as low as possible.  I might experiment with A2DP streaming and mode switching later.  For now I am deprioritizing audio relay over Bluetooth capability.  If anyone reads this and has ideas, please let me know asap!

I'm also looking into audio streaming over TCP or UDP.  I think bidirectionality will be easier to implement this way.  Similar to the concept above, I would use third-party virtual sound card software to forward encoded audio over wifi.

Not sure yet which Windows virtual audio device to use.  PulseAudio seems like a good choice but I'm not familiar with it.  Also, VB-Audio has an app for that, though it doesn't seem to support sample rates lower than 44100 Hz which is much faster than I need.  I could use suggestions..

Summary of Planned Modes:
I've narrowed down to 2 operating modes:  audio relay mode via UDP over wifi, and packet mode with KISS via BLE, TCP, and USB serial.


Research...
04/04/2020 at 00:23 • 0 comments
Where to start?
There are other ways to do this.  

Perhaps the cheapest approach would be to just build custom cables to tap the radio AF-in and PTT pins on the Mic port and AF-out from the SP2 port, wire those directly to my computer's aux-out and mic jacks, and use a software modem.  Gain/level control and PTT may require some circuitry in between.  It is also possible to control the radio (PTT, VFO selection, set frequency, etc) over a TTL serial connection provided on the center ring of the SP2 port.

Similarly, a 2-way Bluetooth audio device could be used to relay AF to and from a PC running sound modem software with the BT device selected as the audio input and output device.  This approach lets you eliminate electrical connections to your control computer.  One could design an analog circuit to pull in PTT when audio is streaming from the controlling PC, or devise some other way to toggle PTT wirelessly, potentially over a separate BT device running as a serial port.  

This is what got me thinking about the ESP32 which could serve as a 2-way BT audio device and either decide digitally when to trigger PTT, or provide a telnet terminal which the control software on the PC can use to command PTT.  Plus, there are tons of other things you can do with an ESP32 driving the radio.

This also got me thinking that the ESP32 is plenty powerful enough to handle onboard AF modulation and demodulation, eliminating the need for a software modem on the PC.  Instead, one could implement KISS protocol over Bluetooth serial profile or over 802.11 telnet to control the TNC and send and receive digital packets.  

This is hardly a new idea, it's just new to me.  Other projects use various forms of microcontrollers with a bluetooth serial interface over I2C or similar.  For instance, the  mobilinkd TNC3 and  MicroModem projects, both of which seem to be well supported and well worth the investment.  It turns out Mobilinkd is also actively looking at moving to the ESP32 but hesitated due to bugs in power management in BLE mode which have since been fixed.

I am very tempted to buy the mobilinkd TNC3 but figured I might as well try building something similar since I have some spare Esp32s and miscellaneous components, plus test equipment, and since various parts of the needed code base already exist in various forms for other platforms.  Also, it turns out @Evan has already started porting Mark Qvist's APRS work to the ESP32.  I'll try Evan's LibAPRS branch and go from there.  There are a few other Arduino KISS TNC projects that also look promising, but are targeted for older, slower boards.  Some look easier than others to port to the ESP32.

If all fails, I will likely break down and buy a mobilinkd TNC3 or wait for an ESP32-based version.

Some thoughts about interfaces:
Strong desire to handle up to 9600 baud on VHF.
(9600 not possible without access to radio modulator)
perhaps I can add this later if people with compatible radios show interest
Priority:  AFSK 1200 bd
maybe BPSK 1200 Bd, BPSK 2400 Bd, QPSK 2400 bps, QPSK 3600 bps?
other modes via soundcard modem, with ESP32 as audio relay
Ideally, the device should also support HF as well, at lower baud.
...in case I get an HF or tri-band rig later, or for other users
Priority:  HF Packet, 1200 baud
other modes via soundcard modem, with ESP32 as audio relay
Support APRS (should be easy since open source libraries are available)
i.g. @Evan 's ESP32 TNC project which ports Mark Qvist's APRS libraries to ESP32
5 4 ways to transfer data between PC and modem:
AF relay mode:  audio to/from PC software modem, 
over Bluetooth (A2DP 2-way?) (deprioritized)
over 802.11 (UDP if TCP not practical)
KISS over bluetooth low energy (BLE) serial profile, or 
KISS over 802.11 TCP
KISS over USB serial port
Wifi (802.11 b/g/n):
Initially will hardcode my home network SSID and password.  Later, will implement a generalized wifi config method similar to most IoT devices, so the modem will default as its own AP but will provide html interface to configure it for any network.
Will provide telnet port to allow terminal computer to configure the radio and send and receive digital data over wifi instead of USB or BT serial.
Will serve an HTML modem and radio configuration page
Will provide HTTP REST server for modem and radio config.
Maybe also use this to send and recieve text messages.  Interesting...
Use the ID-5100 TTL serial port to control VFO selection, squelch, frequency, etc.
Will probably manually code this.  Hamlib looks too heavy.
Not easily adaptable to other radios.  The modem will still work but operator has to manually configure the radio.
Support HTs
Physical interfaces:
Tx "arm" switch?  Breaks ESP32-radio PTT and tx AF links
Use screw terminals for users to manually adapt any hacked-up cables
i.g. cut-up ethernet cable and 3.5mm stereo audio cable for Icom, and a cut-up Baofeng headset for Baofeng HTs (and many others with same interface).
Need onboard regulator to allow variety of power sources (ideally sourced by the radio where possible)
----

AX.25/KISS:
AX.25:  http://www.ax25.net/AX25.2.2-Jul%2098-2.pdf
Good overview:  http://www.ax25.net/kiss.aspx

The AX.25 protocol is removed entirely from the TNC, as are all command interpreters and the like. The TNC simply converts between synchronous HDLC, spoken on the full- or half-duplex radio channel, and a special asynchronous, full duplex frame format spoken on the host/TNC link. Every frame received on the HDLC link is passed intact to the host once it has been translated to the asynchronous format; likewise, asynchronous frames from the host are transmitted on the radio channel once they have been converted to HDLC format.
HDLC:  https://en.wikipedia.org/wiki/High-Level_Data_Link_Control

HDLC frame:
  • Flag (8 bits: b01111110, 0x7E)
  • Address (8+ bits)
  • Control (8 or 16 bits)
  • Information (variable, 8xn bits)
  • Frame Check Sequence (16-bit CRC-CCITT or a 32-bit CRC-32 computed over the Address, Control, and Information fields)
  • Flag (8 bits: b01111110, 0x7E)
If the receiver's calculation of the FCS does not match that of the sender's, indicating that the frame contains errors, the receiver can either send a negative acknowledge packet to the sender, or send nothing. After either receiving a negative acknowledge packet or timing out waiting for a positive acknowledge packet, the sender can retransmit the failed frame.
The FCS field of an AX.25 frame is sent most-significant bit first. All other fields are sent with each octet’s least-significant bit first. 
There are three general types of AX.25 frames:
  • Information frame (I frame)
  • Supervisory frame (S frame)
  • Unnumbered frame (U frame) 
Modulation:

Good summary about modulation techniques used with KISS/AX.25:  https://destevez.net/2016/06/kiss-hdlc-ax-25-and-friends/

[AFSK] is normally used for a rate of 300 baud on the HF bands. The NRZ-I bits are transmitted as an audio signal which frequency shifts between two tones spaced 200Hz apart. The radio is set to SSB mode, so the actual emission is really FSK. The particular tones that are used are not standard, so this has to be compensated by setting the radio dial frequency correctly. It is not important whether LSB or USB mode is used, because the signal is not sensitive to polarity inversion.

The second way to do it is using FM AFSK. This is normally used for a rate of 1200 baud on the VHF and UHF bands. The NZR-I bits are transmitted as an audio signal which frequency shifts between the tones 1200Hz and 2200Hz. This audio signal is FM modulated before transmission.

The third way to do it is using G3RUH FSK [requires direct access to radio modulator].

The fourth way to do it is using BPSK. This is used in a few amateur satellites, using a rate of 1000 or 1200 baud. The NRZ-I bits are transmitted as a BPSK signal (differential encoding is not used). This BPSK signal can be generated as an audio signal on a computer and then used to drive an SSB transmitter.

Finally, fldigi can act as a KISS TNC, allowing to send AX.25 frames in many of the modes supported by this program. However, these digital modes are normally used for text based chat and rarely used for AX.25.

gr-kiss includes example flowgraphs showing how the 1k2 FM AFSK, 9k6 FSK and 1k2 BPSK modulations work.
=>  1200 baud AFSK for VHF/UHF, with tones at 1200Hz and 220Hz.
=>  300 baud FSK on HF SSB (LSB or USB) with 200Hz tone separation.
=>  FLdigi with ESP32 as BT audio relay

Another good summary:  http://www.symek.com/g/pacmod.html


Other modulation methods:
ARDOP P2P HF (use ESP32 as BT audio relay)
FLdigi NBEMS (use ESP32 as BT audio relay)
WinMOR on HF (use ESP32 as BT audio relay)
PACTOR requies proprietary modem >:-(
PSK31 for HF?  No error correction, not recommended for EMCOMM

FLdigi/NBEMS intro:  http://www.arrl.org/files/file/On%20the%20Air/Tutorials/Introduction_to_NBEMS_ARRL.pdf

Fascinating:  https://github.com/daniestevez/gr-satellites


ESP32:
power management tips:  https://www.savjee.be/2019/12/esp32-tips-to-increase-battery-life/

VARA:
main page:  https://rosmodem.wordpress.com/
VARA quick guide:  https://mega.nz/download
=> implement as AF pass-through, driven by PC app

ID-5100a:
Icom ID-5100a service manual:  https://www.iu2frl.it/wp-content/uploads/2019/02/id5100e-service-manual.pdf 

8V regulator:  NJM2835DL1, 500mA max  https://www.njr.com/semicon/PDF/NJM2835_E.pdf
Full manual:  https://www.icomamerica.com/en/downloads/DownloadDocument.aspx?Document=671
SP2 pinout:  tip RXD, Ring: TXD, Sleeve: GND
Serial interface:  4800/9600/19200, Data: 8 bit, Parity: none, Stop: 1 bit, Flow control:Xon/Xoff
Get CI-V address and set baud rate via CI-V menu
Default address:  0x8C
Maybe can also configure Data port as serial?  Pg 12-10; "Data Device Set -> Serialport Function:  CI-V"
Data format on pg 13-16 and Cmd Table on pg 13-17