1.   GHW USB AUDIO (UAC1, VID=0x31B2)
1.a. Playback solo → audio sale limpio, sin distorsión: pasa 
1.b. Full-duplex → input_fx funciona, sin garble (este fue el bug de v1.2.2): pasa
1.c. Hubo una situación al recoger los logs, al principio se distorcionaba el playback en modo chaos pad y en modo input_fx no tomaba el input, luego de mover el dac de posición sin
descontectar o parar el streaaming se arreglo, sospecho que fue una cuestión del hardware y electroacustica, pero adjunto los logs estandidos de toda la situación :
     10:41:11.677  D  UsbAudioManager created
     10:41:11.677  I  Starting USB device monitoring
     10:41:11.678  I  BroadcastReceiver registered (pre-Android 13)
     10:41:11.678  I  Listening for actions: ATTACHED, DETACHED, com.watermellonstudios.audio.USB_PERMISSION
     10:41:11.680  D  Auto-connect enabled
     10:41:11.681  D  Found 0 USB Audio devices
     10:41:20.200  D  USB device attached
     10:41:20.200  D  BroadcastReceiver.onReceive: action=android.hardware.usb.action.USB_DEVICE_ATTACHED
     10:41:20.201  I  USB Audio device attached: GHW USB AUDIO
     10:41:20.201  I    VID=0x31B2, PID=0x0011
     10:41:20.201  I    Manufacturer: GHW Micro
     10:41:20.201  I    Product: GHW USB AUDIO
     10:41:20.201  I    Device ID: 1002
     10:41:20.203  I  USB DEVICE: id=1002, VID=0x31B2, PID=0x0011, product=GHW USB AUDIO, manufacturer=GHW Micro, interfaces=7
     10:41:20.207  I  Compatibility check: COMPATIBLE - Device is in compatibility list
     10:41:20.208  I  RECONNECT: autoConnect=true, selected=null, state=DISCONNECTED, hasPermission=false
     10:41:20.208  I  Compatible device detected, auto-connect: true
     10:41:20.208  I    selectedDevice before auto-connect: null
     10:41:20.208  I    isDeviceReady before auto-connect: false
     10:41:20.208  I  Auto-connecting to GHW Micro GHW USB AUDIO
     10:41:20.208  D  Connecting to device: GHW Micro GHW USB AUDIO
     10:41:20.522  I  Device GHW Micro GHW USB AUDIO is in trusted list
     10:41:20.522  I  Trusted device requires permission again. User should check 'Always use NoisyPad for this device'
     10:41:20.523  I  Trusted device needs re-authorization: GHW Micro GHW USB AUDIO
     10:41:20.523  I  requestPermissionSuspend: Requesting permission for GHW USB AUDIO
     10:41:20.524  I    Action: com.watermellonstudios.audio.USB_PERMISSION
     10:41:20.524  I    Package: com.watermellonstudios.noisypad
     10:41:20.524  I    Flags: 167772160 (FLAG_MUTABLE=33554432, FLAG_UPDATE_CURRENT=134217728)
     10:41:20.524  I    Device: GHW USB AUDIO (id=1002)
     10:41:20.524  I    isMonitoring: true
     10:41:20.537  I  Permission dialog should now appear - waiting for user response...
     10:41:20.682  D    - USB-Audio - GHW USB AUDIO (USB_DAC)
     10:41:20.682  D  Auto-selecting device: USB-Audio - GHW USB AUDIO (USB_DAC)
     10:41:20.682  D  Auto-selected input source: USB_DAC
     10:41:20.708  D    - USB-Audio - GHW USB AUDIO (USB_DAC)
     10:41:20.708  D  Auto-selecting device: USB-Audio - GHW USB AUDIO (USB_DAC)
     10:41:20.708  D  Auto-selected input source: USB_DAC
     10:41:20.708  D  Input device added: USB-Audio - GHW USB AUDIO (type=22)
     10:41:20.709  D    - USB-Audio - GHW USB AUDIO (USB_DAC)
     10:41:20.709  D  Auto-selecting device: USB-Audio - GHW USB AUDIO (USB_DAC)
     10:41:20.709  D  Auto-selected input source: USB_DAC
     10:41:24.214  D  BroadcastReceiver.onReceive: action=com.watermellonstudios.audio.USB_PERMISSION
     10:41:24.214  I  ACTION_USB_PERMISSION received!
     10:41:24.215  I    Intent extras: device, permission
     10:41:24.215  I    device: GHW USB AUDIO (id=1002)
     10:41:24.215  I    granted: true
     10:41:24.215  I    pendingPermissionDevice: GHW USB AUDIO (id=1002)
     10:41:24.215  I    permissionContinuation is null: false
     10:41:24.215  D  handlePermissionResult called: device=GHW USB AUDIO, granted=true
     10:41:24.215  I  Permission result for GHW USB AUDIO: true
     10:41:24.232  I  Added trusted device: GHW Micro GHW USB AUDIO (12722:17)
     10:41:24.232  I  Device GHW Micro GHW USB AUDIO added to trusted devices
     10:41:24.269  I  Connected to GHW Micro GHW USB AUDIO, FD=5
     10:41:24.269  D  Initialize native USB: fd=5, path=/dev/bus/usb/001/002
     10:41:24.269  I  AudioNativeBridge.initializeUsbDevice: fd=5, path=/dev/bus/usb/001/002
     10:41:24.269  I  Initializing USB backend: fd=5, path=/dev/bus/usb/001/002
     10:41:24.269  I  LibusbBackend created
     10:41:24.269  I  Initializing from fd=5, path=/dev/bus/usb/001/002
     10:41:24.270  I  USB Device: VID=0x31B2, PID=0x0011
     10:41:24.271  I  Manufacturer: GHW Micro
     10:41:24.265  W  type=1400 audit(0.0:1151): avc: denied { read } for name="usb" dev="tmpfs" ino=467906 scontext=u:r:untrusted_app:s0:c76,c257,c512,c768 tcontext=u:object_r:usb_device:s0 tclass=dir permissive=0 app=com.watermellonstudios.noisypad
     10:41:24.271  I  Product: GHW USB AUDIO
     10:41:24.271  I  Config descriptor: totalLength=294, configValue=1
     10:41:24.272  I  Got raw config descriptor: 294 bytes
     10:41:24.272  D  Configuration descriptor: 4 interfaces, total length 294
     10:41:24.272  D  Found Audio Control interface: 0
     10:41:24.272  D  Detected UAC 1.0 from bcdADC: 0x0100
     10:41:24.272  D  AC Header: bcdADC=0x0100, bInCollection=2
     10:41:24.272  D  Input Terminal: ID=1, Type=0x0201, Channels=1
     10:41:24.272  D  Feature Unit: ID=2, SourceID=1, ControlSize=1, Channels=0, Mute=1, Volume=1
     10:41:24.272  D  Skipping AC descriptor subtype: 0x05
     10:41:24.272  D  Output Terminal: ID=4, Type=0x0101, SourceID=3
     10:41:24.272  D  Input Terminal: ID=5, Type=0x0101, Channels=2
     10:41:24.272  D  Skipping AC descriptor subtype: 0x04
     10:41:24.272  D  Feature Unit: ID=7, SourceID=6, ControlSize=1, Channels=1, Mute=1, Volume=0
     10:41:24.272  D    Channel 0 controls: 0x00000001 (Mute=1, Volume=0)
     10:41:24.272  D    Channel 1 controls: 0x00000002 (Mute=0, Volume=1)
     10:41:24.272  D  Output Terminal: ID=8, Type=0x0301, SourceID=7
     10:41:24.272  D  Found Audio Streaming interface: 1 alt 0 (endpoints: 0)
     10:41:24.272  D  Found Audio Streaming interface: 1 alt 1 (endpoints: 1)
     10:41:24.272  D  AS General: TerminalLink=4, FormatTag=0x0001, Delay=1
     10:41:24.272  D  Format Type I: Channels=1, SubframeSize=2, BitRes=16, SamFreqType=1
     10:41:24.272  D    Sample rate 0: 48000 Hz
     10:41:24.272  D  Endpoint: Address=0x81, Attr=0x05, MaxPacket=96, Interval=1
     10:41:24.272  D    Isochronous endpoint: IN, Async
     10:41:24.272  D  Audio Endpoint: bmAttr=0x01 (SamFreq=1, Pitch=0, MaxPkt=0)
     10:41:24.272  D  Found Audio Streaming interface: 2 alt 0 (endpoints: 0)
     10:41:24.272  D  Found Audio Streaming interface: 2 alt 1 (endpoints: 1)
     10:41:24.272  D  AS General: TerminalLink=5, FormatTag=0x0001, Delay=1
     10:41:24.272  D  Format Type I: Channels=2, SubframeSize=2, BitRes=16, SamFreqType=2
     10:41:24.272  D    Sample rate 0: 48000 Hz
     10:41:24.272  D    Sample rate 1: 96000 Hz
     10:41:24.272  D  Endpoint: Address=0x01, Attr=0x09, MaxPacket=384, Interval=1
     10:41:24.272  D    Isochronous endpoint: OUT, Adaptive
     10:41:24.272  D  Audio Endpoint: bmAttr=0x01 (SamFreq=1, Pitch=0, MaxPkt=0)
     10:41:24.272  D  Found Audio Streaming interface: 2 alt 2 (endpoints: 1)
     10:41:24.272  D  AS General: TerminalLink=5, FormatTag=0x0001, Delay=1
     10:41:24.272  D  Format Type I: Channels=2, SubframeSize=3, BitRes=24, SamFreqType=2
     10:41:24.272  D    Sample rate 0: 48000 Hz
     10:41:24.272  D    Sample rate 1: 96000 Hz
     10:41:24.272  D  Endpoint: Address=0x01, Attr=0x09, MaxPacket=576, Interval=1
     10:41:24.272  D    Isochronous endpoint: OUT, Adaptive
     10:41:24.272  D  Audio Endpoint: bmAttr=0x01 (SamFreq=1, Pitch=0, MaxPkt=0)
     10:41:24.272  I  Successfully parsed USB Audio device: VID=31b2 PID=0011
     10:41:24.272  I    Playback interfaces: 2, Capture interfaces: 1
     10:41:24.272  I  Parsed USB Audio device:
     10:41:24.272  I    UAC Version: 1
     10:41:24.272  I    Playback interfaces: 2
     10:41:24.272  I    Capture interfaces: 1
     10:41:24.272  I  selectBestInterfaces: mode=PLAYBACK_ONLY, needsPlayback=1, needsCapture=0
     10:41:24.273  I  Available playback altsettings (2):
     10:41:24.273  I    [0] IF2 Alt1: 2ch/16bit (1 fmt) rates={48000,96000} ep=0x01 fb=no
     10:41:24.273  I    [1] IF2 Alt2: 2ch/24bit (1 fmt) rates={48000,96000} ep=0x01 fb=no
     10:41:24.273  I  Selected playback: IF2 Alt2, 48000Hz, 2ch, 24bit, score=1.071
     10:41:24.275  I  Detached kernel driver from control interface 0
     10:41:24.275  I  Claimed AudioControl interface 0
     10:41:24.275  I  UsbVolumeControl: Initializing FU=2, hasVolume=1, hasMute=1, isOutput=1, UAC=1
     10:41:24.276  I  UsbVolumeControl: Volume range: min=-12800 (-50.0 dB), max=0 (0.0 dB), res=128
     10:41:24.276  I  UsbVolumeControl: Hardware volume available, current=0 (0.0 dB)
     10:41:24.276  I  UsbVolumeControl: Hardware mute available, current=0
     10:41:24.276  I  UsbVolumeControl: Initialized - HW volume=1, HW mute=1, range=[-12800, 0] res=128
     10:41:24.276  I  Output volume control: HW=1, Volume=1, Mute=1, FU=2
     10:41:24.276  I  USB device initialized successfully
     10:41:24.276  I  USB backend initialized successfully
     10:41:24.276  I  AudioNativeBridge.initializeUsbDevice: LibusbBackend initialized successfully
     10:41:24.276  I  Native USB device initialized successfully
     10:41:24.277  D  USB Capabilities parsed: [2.0, 1.0, 96000.0, 3.0, 2.0, 1.0, 2.0]
     10:41:24.277  I  Auto-connect result: Success(value=kotlin.Unit)
     10:41:24.278  I    isDeviceReady after auto-connect: true
     10:41:24.278  I  Auto-connect successful, emitting CompatibleDeviceDetected
     10:41:24.278  I  Compatible USB device detected: GHW Micro GHW USB AUDIO
     10:41:24.278  I  Device ready immediately, auto-switching to USB backend
     10:41:24.278  I  Backend selected: USB Audio
     10:41:24.278  I  Switched to USB Direct backend
     10:41:24.279  I  Emitting DeviceConnected event
     10:41:24.279  I  Auto-switched to USB backend successfully
     10:41:24.279  D  USB capabilities updated: hasCapture=true, supportsFullDuplex=true
     10:41:26.338  I  App launched/resumed via USB_DEVICE_ATTACHED intent
     10:41:26.338  I    Device: GHW USB AUDIO (VID=12722, PID=17)
     10:41:26.340  I  handleUsbDeviceFromIntent: deviceId=1002
     10:41:26.348  I  USB DEVICE: id=1002, VID=0x31B2, PID=0x0011, product=GHW USB AUDIO, manufacturer=GHW Micro, interfaces=7
     10:41:26.348  D  Found 1 USB Audio devices
     10:41:26.466  I  Found device from intent: GHW Micro GHW USB AUDIO
     10:42:22.688  D  TogglePlay: Using LIBUSB backend
     10:42:22.692  I  selectBestInterfaces: mode=PLAYBACK_ONLY, needsPlayback=1, needsCapture=0
     10:42:22.692  I  Available playback altsettings (2):
     10:42:22.692  I    [0] IF2 Alt1: 2ch/16bit (1 fmt) rates={48000,96000} ep=0x01 fb=no
     10:42:22.692  I    [1] IF2 Alt2: 2ch/24bit (1 fmt) rates={48000,96000} ep=0x01 fb=no
     10:42:22.692  I  Selected playback: IF2 Alt2, 48000Hz, 2ch, 24bit, score=1.071
     10:42:22.692  I  Starting with full-duplex=false, capture selected=no
     10:42:22.692  I  UsbTransferManager UAC version set to 1
     10:42:22.692  I  USB speed: FULL (libusb=2) → 1 packets/ms, 48 frames/packet, 8 packets/xfer
     10:42:22.692  I  Ring buffer config: ringBufferMs=100, outputSize=9600 samples (38400 bytes), inputSize=9600 samples (38400 bytes)
     10:42:22.693  I  Configured: 48000Hz, out=2ch/24bit, in=2ch/24bit, 48 frames/packet, 8 packets/xfer
     10:42:22.693  I  Output interface set: IF2 Alt2, EP 0x01
     10:42:22.694  I  Detached kernel driver from interface 2
     10:42:22.695  I  Claimed interface 2
     10:42:22.701  I  Set interface 2 to alt setting 2
     10:42:22.702  I  Rate negotiation: UAC1 EP 0x01 req=48000 actual=48000
     10:42:22.702  I  Allocating output transfers: nominal=288, clockMargin=+24, endpoint wMaxPacketSize=0x0240 (effective=576, informational), buffer stride=312 bytes/packet
     10:42:22.703  I  Started USB transfers
     10:42:22.703  I  USB event loop started
     10:42:22.704  I  Pre-allocated DSP buffers: frames=256, outCh=2(512 samples), inCh=0(256 samples), monoToStereo=0
     10:42:22.704  I  LibusbBackend started
     10:42:22.704  I  Backend started: USB Audio
     10:42:22.704  I    Backend type: USB Audio
     10:42:22.704  I  DSP thread started, mode=PLAYBACK_ONLY
     10:42:22.704  I  Event loop thread pinned to core 7 (of 8)
     10:42:22.707  I  DSP thread pinned to core 6 (of 8)
     10:42:23.545  I  START_USB_FADE: sampleRate=48000, fadeTimeMs=500
     10:42:25.087  I  USB_DSP: hasCapture=0 inputPtr=0x0 outputPtr=0xb4000079ce940000 frames=256 streamMode=0 | read ok=0 fail=0 ratio=0.00 ringAvailPre=0 lastPeak=0.00000
     10:42:25.087  I  USB_CB: inputData=0x0, oscEnabled=1, isUsbInputFxMode=0, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00000
     10:42:27.489  I  USB_DSP: hasCapture=0 inputPtr=0x0 outputPtr=0xb4000079ce940000 frames=256 streamMode=0 | read ok=0 fail=0 ratio=0.00 ringAvailPre=0 lastPeak=0.00000
     10:42:27.489  I  USB_CB: inputData=0x0, oscEnabled=1, isUsbInputFxMode=0, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00000
     10:42:29.887  I  USB_DSP: hasCapture=0 inputPtr=0x0 outputPtr=0xb4000079ce940000 frames=256 streamMode=0 | read ok=0 fail=0 ratio=0.00 ringAvailPre=0 lastPeak=0.00000
     10:42:29.887  I  USB_CB: inputData=0x0, oscEnabled=1, isUsbInputFxMode=0, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00000
     10:42:32.287  I  USB_DSP: hasCapture=0 inputPtr=0x0 outputPtr=0xb4000079ce940000 frames=256 streamMode=0 | read ok=0 fail=0 ratio=0.00 ringAvailPre=0 lastPeak=0.00000
     10:42:32.287  I  USB_CB: inputData=0x0, oscEnabled=1, isUsbInputFxMode=0, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00000
     10:42:34.663  I  USB_DSP: hasCapture=0 inputPtr=0x0 outputPtr=0xb4000079ce940000 frames=256 streamMode=0 | read ok=0 fail=0 ratio=0.00 ringAvailPre=0 lastPeak=0.00000
     10:42:34.663  I  USB_CB: inputData=0x0, oscEnabled=1, isUsbInputFxMode=0, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00000
     10:42:37.065  I  USB_DSP: hasCapture=0 inputPtr=0x0 outputPtr=0xb4000079ce940000 frames=256 streamMode=0 | read ok=0 fail=0 ratio=0.00 ringAvailPre=0 lastPeak=0.00000
     10:42:37.065  I  USB_CB: inputData=0x0, oscEnabled=1, isUsbInputFxMode=0, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00000
     10:42:39.457  I  USB_DSP: hasCapture=0 inputPtr=0x0 outputPtr=0xb4000079ce940000 frames=256 streamMode=0 | read ok=0 fail=0 ratio=0.00 ringAvailPre=0 lastPeak=0.00000
     10:42:39.457  I  USB_CB: inputData=0x0, oscEnabled=1, isUsbInputFxMode=0, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00000
     10:42:41.855  I  USB_DSP: hasCapture=0 inputPtr=0x0 outputPtr=0xb4000079ce940000 frames=256 streamMode=0 | read ok=0 fail=0 ratio=0.00 ringAvailPre=0 lastPeak=0.00000
     10:42:41.855  I  USB_CB: inputData=0x0, oscEnabled=1, isUsbInputFxMode=0, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00000
     10:42:44.247  I  USB_DSP: hasCapture=0 inputPtr=0x0 outputPtr=0xb4000079ce940000 frames=256 streamMode=0 | read ok=0 fail=0 ratio=0.00 ringAvailPre=0 lastPeak=0.00000
     10:42:44.247  I  USB_CB: inputData=0x0, oscEnabled=1, isUsbInputFxMode=0, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00000
     10:42:46.647  I  USB_DSP: hasCapture=0 inputPtr=0x0 outputPtr=0xb4000079ce940000 frames=256 streamMode=0 | read ok=0 fail=0 ratio=0.00 ringAvailPre=0 lastPeak=0.00000
     10:42:46.647  I  USB_CB: inputData=0x0, oscEnabled=1, isUsbInputFxMode=0, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00000
     10:42:49.047  I  USB_DSP: hasCapture=0 inputPtr=0x0 outputPtr=0xb4000079ce940000 frames=256 streamMode=0 | read ok=0 fail=0 ratio=0.00 ringAvailPre=0 lastPeak=0.00000
     10:42:49.047  I  USB_CB: inputData=0x0, oscEnabled=1, isUsbInputFxMode=0, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00000
     10:42:51.447  I  USB_DSP: hasCapture=0 inputPtr=0x0 outputPtr=0xb4000079ce940000 frames=256 streamMode=0 | read ok=0 fail=0 ratio=0.00 ringAvailPre=0 lastPeak=0.00000
     10:42:51.447  I  USB_CB: inputData=0x0, oscEnabled=1, isUsbInputFxMode=0, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00000
     10:42:53.849  I  USB_DSP: hasCapture=0 inputPtr=0x0 outputPtr=0xb4000079ce940000 frames=256 streamMode=0 | read ok=0 fail=0 ratio=0.00 ringAvailPre=0 lastPeak=0.00000
     10:42:53.849  I  USB_CB: inputData=0x0, oscEnabled=1, isUsbInputFxMode=0, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00000
     10:42:56.239  I  USB_DSP: hasCapture=0 inputPtr=0x0 outputPtr=0xb4000079ce940000 frames=256 streamMode=0 | read ok=0 fail=0 ratio=0.00 ringAvailPre=0 lastPeak=0.00000
     10:42:56.239  I  USB_CB: inputData=0x0, oscEnabled=1, isUsbInputFxMode=0, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00000
     10:42:58.641  I  USB_DSP: hasCapture=0 inputPtr=0x0 outputPtr=0xb4000079ce940000 frames=256 streamMode=0 | read ok=0 fail=0 ratio=0.00 ringAvailPre=0 lastPeak=0.00000
     10:42:58.641  I  USB_CB: inputData=0x0, oscEnabled=1, isUsbInputFxMode=0, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00000
     10:43:01.041  I  USB_DSP: hasCapture=0 inputPtr=0x0 outputPtr=0xb4000079ce940000 frames=256 streamMode=0 | read ok=0 fail=0 ratio=0.00 ringAvailPre=0 lastPeak=0.00000
     10:43:01.041  I  USB_CB: inputData=0x0, oscEnabled=1, isUsbInputFxMode=0, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00000
     10:43:03.431  I  USB_DSP: hasCapture=0 inputPtr=0x0 outputPtr=0xb4000079ce940000 frames=256 streamMode=0 | read ok=0 fail=0 ratio=0.00 ringAvailPre=0 lastPeak=0.00000
     10:43:03.431  I  USB_CB: inputData=0x0, oscEnabled=1, isUsbInputFxMode=0, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00000
     10:43:05.831  I  USB_DSP: hasCapture=0 inputPtr=0x0 outputPtr=0xb4000079ce940000 frames=256 streamMode=0 | read ok=0 fail=0 ratio=0.00 ringAvailPre=0 lastPeak=0.00000
     10:43:05.831  I  USB_CB: inputData=0x0, oscEnabled=1, isUsbInputFxMode=0, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00000
     10:43:08.231  I  USB_DSP: hasCapture=0 inputPtr=0x0 outputPtr=0xb4000079ce940000 frames=256 streamMode=0 | read ok=0 fail=0 ratio=0.00 ringAvailPre=0 lastPeak=0.00000
     10:43:08.231  I  USB_CB: inputData=0x0, oscEnabled=1, isUsbInputFxMode=0, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00000
     10:43:10.631  I  USB_DSP: hasCapture=0 inputPtr=0x0 outputPtr=0xb4000079ce940000 frames=256 streamMode=0 | read ok=0 fail=0 ratio=0.00 ringAvailPre=0 lastPeak=0.00000
     10:43:10.631  I  USB_CB: inputData=0x0, oscEnabled=1, isUsbInputFxMode=0, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00000
     10:43:13.023  I  USB_DSP: hasCapture=0 inputPtr=0x0 outputPtr=0xb4000079ce940000 frames=256 streamMode=0 | read ok=0 fail=0 ratio=0.00 ringAvailPre=0 lastPeak=0.00000
     10:43:13.023  I  USB_CB: inputData=0x0, oscEnabled=1, isUsbInputFxMode=0, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00000
     10:43:13.259  I  prepareForInputMode: currentBackend=LIBUSB, usbAvailable=true
     10:43:13.259  I  prepareForInputMode: USB active, ensuring FULL_DUPLEX
     10:43:13.260  I  Stopping LibusbBackend...
     10:43:13.260  I  DSP thread stopped
     10:43:13.261  I  Stopping USB transfers...
     10:43:13.262  I  USB event loop stopped, disconnected=0
     10:43:13.277  I  Released interface 2
     10:43:13.277  I  Stopped USB transfers. Stats: submitted=50576, completed=50552, errors=0, underruns=2098
     10:43:13.277  I  LibusbBackend stopped, deviceDisconnected=0
     10:43:13.481  I  selectBestInterfaces: mode=FULL_DUPLEX, needsPlayback=1, needsCapture=1
     10:43:13.481  I  Available playback altsettings (2):
     10:43:13.481  I    [0] IF2 Alt1: 2ch/16bit (1 fmt) rates={48000,96000} ep=0x01 fb=no
     10:43:13.481  I    [1] IF2 Alt2: 2ch/24bit (1 fmt) rates={48000,96000} ep=0x01 fb=no
     10:43:13.481  I  Selected playback: IF2 Alt2, 48000Hz, 2ch, 24bit, score=1.071
     10:43:13.481  I  Available capture altsettings (1):
     10:43:13.481  I    [0] IF1 Alt1: 1ch/16bit (1 fmt) rates={48000} ep=0x81 fb=no
     10:43:13.481  W  No scoring capture candidate; using IF1 Alt1 as last-resort fallback (1ch/16bit @ 48000Hz)
     10:43:13.481  I  Starting with full-duplex=true, capture selected=yes
     10:43:13.481  I  Tearing down previous transfer manager before restart
     10:43:13.482  I  UsbTransferManager UAC version set to 1
     10:43:13.482  I  Input config: 1 channels, 16-bit (output: 2 channels, 24-bit)
     10:43:13.482  I  USB speed: FULL (libusb=2) → 1 packets/ms, 48 frames/packet, 8 packets/xfer
     10:43:13.482  I  Ring buffer config: ringBufferMs=100, outputSize=9600 samples (38400 bytes), inputSize=4800 samples (19200 bytes)
     10:43:13.483  I  Configured: 48000Hz, out=2ch/24bit, in=1ch/16bit, 48 frames/packet, 8 packets/xfer
     10:43:13.483  I  Output interface set: IF2 Alt2, EP 0x01
     10:43:13.483  I  Input interface set: IF1 Alt1, EP 0x81
     10:43:13.484  I  Claimed interface 2
     10:43:13.501  I  Set interface 2 to alt setting 2
     10:43:13.502  I  Detached kernel driver from interface 1
     10:43:13.502  I  Claimed interface 1
     10:43:13.517  I  Set interface 1 to alt setting 1
     10:43:13.518  I  Rate negotiation: UAC1 EP 0x01 req=48000 actual=48000
     10:43:13.518  I  Allocating output transfers: nominal=288, clockMargin=+24, endpoint wMaxPacketSize=0x0240 (effective=576, informational), buffer stride=312 bytes/packet
     10:43:13.518  I  Allocating input transfers:  nominal=96, endpoint wMaxPacketSize=0x0060 (effective=96), stride=96 bytes/packet
     10:43:13.518  I  Allocating input transfers: size=768 bytes (8 packets × 96 bytes/packet)
     10:43:13.519  I  Started USB transfers
     10:43:13.519  I  USB event loop started
     10:43:13.519  I  Pre-allocated DSP buffers: frames=256, outCh=2(512 samples), inCh=1(256 samples), monoToStereo=1
     10:43:13.519  I  LibusbBackend started
     10:43:13.519  I  Backend started: USB Audio
     10:43:13.519  I    Backend type: USB Audio
     10:43:13.520  I  DSP thread started, mode=FULL_DUPLEX
     10:43:13.520  I  DSP thread pinned to core 6 (of 8)
     10:43:13.530  I  Event loop thread pinned to core 7 (of 8)
     10:43:14.350  I  START_USB_FADE: sampleRate=48000, fadeTimeMs=200
     10:43:14.684  I  SET_MODE_INPUT_FX: inputNode=0xb400007b228dee58, backendType=2, isUsbActive=1
     10:43:14.685  I  AudioNativeBridge.setAudioMode: USB active, stopping Oboe input
     10:43:14.971  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=271 fail=0 ratio=1.00 ringAvailPre=336 lastPeak=0.00334
     10:43:14.971  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00334
     10:43:16.268  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.00393, effects=2
     10:43:16.572  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=336 lastPeak=0.00330
     10:43:16.572  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00330
     10:43:17.327  I  USB DIRECT INPUT_FX: Processing 256 frames directly (no InputNode buffering)
     10:43:17.867  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.00377, effects=2
     10:43:18.172  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=336 lastPeak=0.00337
     10:43:18.172  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00337
     10:43:19.467  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.00342, effects=2
     10:43:19.771  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=336 lastPeak=0.00063
     10:43:19.771  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00060
     10:43:19.995  I  USB DIRECT INPUT_FX: Processing 256 frames directly (no InputNode buffering)
     10:43:21.069  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.00474, effects=2
     10:43:21.372  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=336 lastPeak=0.00095
     10:43:21.372  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00082
     10:43:22.663  I  USB DIRECT INPUT_FX: Processing 256 frames directly (no InputNode buffering)
     10:43:22.667  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.00567, effects=2
     10:43:22.972  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=336 lastPeak=0.00201
     10:43:22.972  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00164
     10:43:24.267  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.00550, effects=2
     10:43:24.571  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=336 lastPeak=0.00345
     10:43:24.571  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00343
     10:43:25.329  I  USB DIRECT INPUT_FX: Processing 256 frames directly (no InputNode buffering)
     10:43:25.869  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.00441, effects=2
     10:43:26.171  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=336 lastPeak=0.00265
     10:43:26.171  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00265
     10:43:27.466  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.00554, effects=2
     10:43:27.772  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=336 lastPeak=0.00224
     10:43:27.772  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00224
     10:43:27.995  I  USB DIRECT INPUT_FX: Processing 256 frames directly (no InputNode buffering)
     10:43:29.067  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.00560, effects=2
     10:43:29.371  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=336 lastPeak=0.00190
     10:43:29.371  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00190
     10:43:30.663  I  USB DIRECT INPUT_FX: Processing 256 frames directly (no InputNode buffering)
     10:43:30.667  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.00616, effects=2
     10:43:30.971  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=336 lastPeak=0.00170
     10:43:30.971  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00170
     10:43:32.267  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.00495, effects=2
     10:43:32.573  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=336 lastPeak=0.00179
     10:43:32.573  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00179
     10:43:33.328  I  USB DIRECT INPUT_FX: Processing 256 frames directly (no InputNode buffering)
     10:43:33.868  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.00447, effects=2
     10:43:34.172  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=336 lastPeak=0.00166
     10:43:34.172  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00166
     10:43:35.468  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.00569, effects=2
     10:43:35.771  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=336 lastPeak=0.00175
     10:43:35.772  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00175
     10:43:35.995  I  USB DIRECT INPUT_FX: Processing 256 frames directly (no InputNode buffering)
     10:43:37.067  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.00519, effects=2
     10:43:37.373  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=336 lastPeak=0.00153
     10:43:37.373  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00153
     10:43:38.663  I  USB DIRECT INPUT_FX: Processing 256 frames directly (no InputNode buffering)
     10:43:38.667  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.00429, effects=2
     10:43:38.971  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=336 lastPeak=0.00127
     10:43:38.971  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00127
     10:43:40.268  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.00298, effects=2
     10:43:40.572  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=336 lastPeak=0.00112
     10:43:40.573  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00112
     10:43:41.327  I  USB DIRECT INPUT_FX: Processing 256 frames directly (no InputNode buffering)
     10:43:41.867  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.00174, effects=2
     10:43:42.171  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=336 lastPeak=0.00101
     10:43:42.171  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00101
     10:43:43.468  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.00111, effects=2
     10:43:43.771  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=336 lastPeak=0.00082
     10:43:43.771  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00082
     10:43:43.994  I  USB DIRECT INPUT_FX: Processing 256 frames directly (no InputNode buffering)
     10:43:45.069  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.00107, effects=2
     10:43:45.372  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=336 lastPeak=0.00039
     10:43:45.372  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00039
     10:43:46.663  I  USB DIRECT INPUT_FX: Processing 256 frames directly (no InputNode buffering)
     10:43:46.667  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.00121, effects=2
     10:43:46.971  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=336 lastPeak=0.00067
     10:43:46.971  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00058
     10:43:48.267  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.00166, effects=2
     10:43:48.571  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=336 lastPeak=0.00129
     10:43:48.571  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00084
     10:43:49.327  I  USB DIRECT INPUT_FX: Processing 256 frames directly (no InputNode buffering)
     10:43:49.868  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.00188, effects=2
     10:43:50.171  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=336 lastPeak=0.00136
     10:43:50.171  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00117
     10:43:51.467  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.00264, effects=2
     10:43:51.771  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=336 lastPeak=0.00175
     10:43:51.771  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00138
     10:43:51.995  I  USB DIRECT INPUT_FX: Processing 256 frames directly (no InputNode buffering)
     10:43:53.067  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.00290, effects=2
     10:43:53.373  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=336 lastPeak=0.00235
     10:43:53.373  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00211
     10:43:54.663  I  USB DIRECT INPUT_FX: Processing 256 frames directly (no InputNode buffering)
     10:43:54.667  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.00229, effects=2
     10:43:54.972  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=336 lastPeak=0.00216
     10:43:54.972  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00216
     10:43:56.266  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.00169, effects=2
     10:43:56.572  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=336 lastPeak=0.00242
     10:43:56.573  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00209
     10:43:57.328  I  USB DIRECT INPUT_FX: Processing 256 frames directly (no InputNode buffering)
     10:43:57.869  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.00127, effects=2
     10:43:57.933  I  Stopping LibusbBackend...
     10:43:57.933  I  DSP thread stopped
     10:43:57.934  I  Stopping USB transfers...
     10:43:57.935  I  USB event loop stopped, disconnected=0
     10:43:57.949  I  Released interface 2
     10:43:57.966  I  Released interface 1
     10:43:57.966  I  Stopped USB transfers. Stats: submitted=44432, completed=88824, errors=0, underruns=0
     10:43:57.966  I  LibusbBackend stopped, deviceDisconnected=0
     10:43:59.219  D  TogglePlay: Using LIBUSB backend
     10:43:59.220  I  selectBestInterfaces: mode=FULL_DUPLEX, needsPlayback=1, needsCapture=1
     10:43:59.220  I  Available playback altsettings (2):
     10:43:59.220  I    [0] IF2 Alt1: 2ch/16bit (1 fmt) rates={48000,96000} ep=0x01 fb=no
     10:43:59.220  I    [1] IF2 Alt2: 2ch/24bit (1 fmt) rates={48000,96000} ep=0x01 fb=no
     10:43:59.220  I  Selected playback: IF2 Alt2, 48000Hz, 2ch, 24bit, score=1.071
     10:43:59.220  I  Available capture altsettings (1):
     10:43:59.220  I    [0] IF1 Alt1: 1ch/16bit (1 fmt) rates={48000} ep=0x81 fb=no
     10:43:59.220  W  No scoring capture candidate; using IF1 Alt1 as last-resort fallback (1ch/16bit @ 48000Hz)
     10:43:59.220  I  Starting with full-duplex=true, capture selected=yes
     10:43:59.220  I  Tearing down previous transfer manager before restart
     10:43:59.221  I  UsbTransferManager UAC version set to 1
     10:43:59.221  I  Input config: 1 channels, 16-bit (output: 2 channels, 24-bit)
     10:43:59.221  I  USB speed: FULL (libusb=2) → 1 packets/ms, 48 frames/packet, 8 packets/xfer
     10:43:59.221  I  Ring buffer config: ringBufferMs=100, outputSize=9600 samples (38400 bytes), inputSize=4800 samples (19200 bytes)
     10:43:59.222  I  Configured: 48000Hz, out=2ch/24bit, in=1ch/16bit, 48 frames/packet, 8 packets/xfer
     10:43:59.222  I  Output interface set: IF2 Alt2, EP 0x01
     10:43:59.222  I  Input interface set: IF1 Alt1, EP 0x81
     10:43:59.222  I  Claimed interface 2
     10:43:59.229  I  Set interface 2 to alt setting 2
     10:43:59.229  I  Claimed interface 1
     10:43:59.246  I  Set interface 1 to alt setting 1
     10:43:59.247  I  Rate negotiation: UAC1 EP 0x01 req=48000 actual=48000
     10:43:59.247  I  Allocating output transfers: nominal=288, clockMargin=+24, endpoint wMaxPacketSize=0x0240 (effective=576, informational), buffer stride=312 bytes/packet
     10:43:59.247  I  Allocating input transfers:  nominal=96, endpoint wMaxPacketSize=0x0060 (effective=96), stride=96 bytes/packet
     10:43:59.247  I  Allocating input transfers: size=768 bytes (8 packets × 96 bytes/packet)
     10:43:59.248  I  Started USB transfers
     10:43:59.248  I  Pre-allocated DSP buffers: frames=256, outCh=2(512 samples), inCh=1(256 samples), monoToStereo=1
     10:43:59.248  I  USB event loop started
     10:43:59.248  I  LibusbBackend started
     10:43:59.248  I  Backend started: USB Audio
     10:43:59.248  I    Backend type: USB Audio
     10:43:59.249  I  DSP thread started, mode=FULL_DUPLEX
     10:43:59.249  I  DSP thread pinned to core 6 (of 8)
     10:43:59.257  I  Event loop thread pinned to core 7 (of 8)
     10:43:59.491  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=45 fail=0 ratio=1.00 ringAvailPre=256 lastPeak=0.00108
     10:43:59.491  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=0.000, masterVol=1.000, inputPeak=0.00108
     10:44:00.091  I  START_USB_FADE: sampleRate=48000, fadeTimeMs=500
     10:44:00.349  I  Stopping LibusbBackend...
     10:44:00.350  I  DSP thread stopped
     10:44:00.350  I  Stopping USB transfers...
     10:44:00.351  I  USB event loop stopped, disconnected=0
     10:44:00.365  I  Released interface 2
     10:44:00.382  I  Released interface 1
     10:44:00.382  I  Stopped USB transfers. Stats: submitted=1120, completed=2192, errors=0, underruns=0
     10:44:00.382  I  LibusbBackend stopped, deviceDisconnected=0
     10:44:02.113  D  TogglePlay: Using LIBUSB backend
     10:44:02.115  I  selectBestInterfaces: mode=FULL_DUPLEX, needsPlayback=1, needsCapture=1
     10:44:02.115  I  Available playback altsettings (2):
     10:44:02.115  I    [0] IF2 Alt1: 2ch/16bit (1 fmt) rates={48000,96000} ep=0x01 fb=no
     10:44:02.115  I    [1] IF2 Alt2: 2ch/24bit (1 fmt) rates={48000,96000} ep=0x01 fb=no
     10:44:02.115  I  Selected playback: IF2 Alt2, 48000Hz, 2ch, 24bit, score=1.071
     10:44:02.115  I  Available capture altsettings (1):
     10:44:02.115  I    [0] IF1 Alt1: 1ch/16bit (1 fmt) rates={48000} ep=0x81 fb=no
     10:44:02.115  W  No scoring capture candidate; using IF1 Alt1 as last-resort fallback (1ch/16bit @ 48000Hz)
     10:44:02.115  I  Starting with full-duplex=true, capture selected=yes
     10:44:02.115  I  Tearing down previous transfer manager before restart
     10:44:02.115  I  UsbTransferManager UAC version set to 1
     10:44:02.115  I  Input config: 1 channels, 16-bit (output: 2 channels, 24-bit)
     10:44:02.115  I  USB speed: FULL (libusb=2) → 1 packets/ms, 48 frames/packet, 8 packets/xfer
     10:44:02.115  I  Ring buffer config: ringBufferMs=100, outputSize=9600 samples (38400 bytes), inputSize=4800 samples (19200 bytes)
     10:44:02.116  I  Configured: 48000Hz, out=2ch/24bit, in=1ch/16bit, 48 frames/packet, 8 packets/xfer
     10:44:02.116  I  Output interface set: IF2 Alt2, EP 0x01
     10:44:02.116  I  Input interface set: IF1 Alt1, EP 0x81
     10:44:02.117  I  Claimed interface 2
     10:44:02.125  I  Set interface 2 to alt setting 2
     10:44:02.125  I  Claimed interface 1
     10:44:02.142  I  Set interface 1 to alt setting 1
     10:44:02.143  I  Rate negotiation: UAC1 EP 0x01 req=48000 actual=48000
     10:44:02.143  I  Allocating output transfers: nominal=288, clockMargin=+24, endpoint wMaxPacketSize=0x0240 (effective=576, informational), buffer stride=312 bytes/packet
     10:44:02.143  I  Allocating input transfers:  nominal=96, endpoint wMaxPacketSize=0x0060 (effective=96), stride=96 bytes/packet
     10:44:02.143  I  Allocating input transfers: size=768 bytes (8 packets × 96 bytes/packet)
     10:44:02.144  I  Started USB transfers
     10:44:02.144  I  USB event loop started
     10:44:02.144  I  Pre-allocated DSP buffers: frames=256, outCh=2(512 samples), inCh=1(256 samples), monoToStereo=1
     10:44:02.144  I  LibusbBackend started
     10:44:02.144  I  Backend started: USB Audio
     10:44:02.144  I    Backend type: USB Audio
     10:44:02.144  I  DSP thread started, mode=FULL_DUPLEX
     10:44:02.144  I  Event loop thread pinned to core 7 (of 8)
     10:44:02.145  I  DSP thread pinned to core 6 (of 8)
     10:44:02.587  I  USB_DIRECT_OUT: gainStart=0.000, gainEnd=0.000, fadeStart=0.000, fadeEnd=0.000, masterVol=1.000, outPeak=0.00000, effects=2
     10:44:02.897  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=140 fail=0 ratio=1.00 ringAvailPre=512 lastPeak=0.00239
     10:44:02.897  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=0.000, masterVol=1.000, inputPeak=0.00142
     10:44:02.952  I  START_USB_FADE: sampleRate=48000, fadeTimeMs=500
     10:44:03.120  I  USB DIRECT INPUT_FX: Processing 256 frames directly (no InputNode buffering)
     10:44:04.188  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.00118, effects=2
     10:44:04.497  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=512 lastPeak=0.00302
     10:44:04.497  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00248
     10:44:05.785  I  USB DIRECT INPUT_FX: Processing 256 frames directly (no InputNode buffering)
     10:44:05.789  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.00163, effects=2
     10:44:06.096  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=512 lastPeak=0.00270
     10:44:06.096  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00203
     10:44:07.389  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.00147, effects=2
     10:44:07.695  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=512 lastPeak=0.00302
     10:44:07.696  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00242
     10:44:08.452  I  USB DIRECT INPUT_FX: Processing 256 frames directly (no InputNode buffering)
     10:44:08.987  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.00172, effects=2
     10:44:09.296  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=512 lastPeak=0.00268
     10:44:09.296  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00222
     10:44:10.588  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.00151, effects=2
     10:44:10.896  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=512 lastPeak=0.00291
     10:44:10.896  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00259
     10:44:11.120  I  USB DIRECT INPUT_FX: Processing 256 frames directly (no InputNode buffering)
     10:44:12.188  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.00174, effects=2
     10:44:12.496  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=512 lastPeak=0.00291
     10:44:12.496  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00291
     10:44:13.784  I  USB DIRECT INPUT_FX: Processing 256 frames directly (no InputNode buffering)
     10:44:13.784  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.00533, effects=2
     10:44:14.096  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=512 lastPeak=0.00654
     10:44:14.096  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00654
     10:44:15.384  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.00496, effects=2
     10:44:15.697  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=512 lastPeak=0.00673
     10:44:15.697  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00673
     10:44:16.448  I  USB DIRECT INPUT_FX: Processing 256 frames directly (no InputNode buffering)
     10:44:16.984  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.00436, effects=2
     10:44:17.296  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=512 lastPeak=0.13379
     10:44:17.296  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.13379
     10:44:18.584  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.02820, effects=2
     10:44:18.897  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=512 lastPeak=0.06579
     10:44:18.897  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.06579
     10:44:19.121  I  USB DIRECT INPUT_FX: Processing 256 frames directly (no InputNode buffering)
     10:44:20.184  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.18225, effects=2
     10:44:20.497  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=512 lastPeak=0.13366
     10:44:20.497  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.10689
     10:44:21.784  I  USB DIRECT INPUT_FX: Processing 256 frames directly (no InputNode buffering)
     10:44:21.787  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.03082, effects=2
     10:44:22.096  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=512 lastPeak=0.02902
     10:44:22.096  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.01318
     10:44:23.388  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.22651, effects=2
     10:44:23.696  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=512 lastPeak=0.16378
     10:44:23.697  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.14477
     10:44:24.453  I  USB DIRECT INPUT_FX: Processing 256 frames directly (no InputNode buffering)
     10:44:24.990  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.17496, effects=2
     10:44:25.298  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=512 lastPeak=0.19828
     10:44:25.298  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.10659
     10:44:26.588  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.10202, effects=2
     10:44:26.896  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=512 lastPeak=0.04909
     10:44:26.896  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.04393
     10:44:27.121  I  USB DIRECT INPUT_FX: Processing 256 frames directly (no InputNode buffering)
     10:44:28.188  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.06240, effects=2
     10:44:28.498  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=512 lastPeak=0.10139
     10:44:28.498  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.10139
     10:44:29.785  I  USB DIRECT INPUT_FX: Processing 256 frames directly (no InputNode buffering)
     10:44:29.790  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.10890, effects=2
     10:44:30.096  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=512 lastPeak=0.20368
     10:44:30.096  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.20368
     10:44:31.388  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.07865, effects=2
     10:44:31.696  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=512 lastPeak=0.42405
     10:44:31.696  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.10542
     10:44:32.453  I  USB DIRECT INPUT_FX: Processing 256 frames directly (no InputNode buffering)
     10:44:32.988  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.12021, effects=2
     10:44:33.296  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=512 lastPeak=0.03129
     10:44:33.296  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00826
     10:44:34.588  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.04534, effects=2
     10:44:34.897  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=512 lastPeak=0.11537
     10:44:34.897  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.11537
     10:44:35.120  I  USB DIRECT INPUT_FX: Processing 256 frames directly (no InputNode buffering)
     10:44:36.188  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.06672, effects=2
     10:44:36.496  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=512 lastPeak=0.00332
     10:44:36.496  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00324
     10:44:37.784  I  USB DIRECT INPUT_FX: Processing 256 frames directly (no InputNode buffering)
     10:44:37.788  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.00824, effects=2
     10:44:38.096  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=512 lastPeak=0.00600
     10:44:38.096  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=1, isUsbInputFxMode=0, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00570
     10:44:39.698  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=512 lastPeak=0.01599
     10:44:39.698  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=1, isUsbInputFxMode=0, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.01579
     10:44:40.592  I  USB FEED: 256 frames, maxSample=0.0305, ringAvail=95744, monitorEnabled=0, monitorAvail=0
     10:44:40.592  I  USB MIX/VOCODER: Fed 256 frames to InputNode
     10:44:41.297  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=512 lastPeak=0.00406
     10:44:41.297  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=1, isUsbInputFxMode=0, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00406
     10:44:42.897  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=512 lastPeak=0.00507
     10:44:42.897  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=1, isUsbInputFxMode=0, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00507
     10:44:43.260  I  USB FEED: 256 frames, maxSample=0.0194, ringAvail=95744, monitorEnabled=0, monitorAvail=0
     10:44:43.260  I  USB MIX/VOCODER: Fed 256 frames to InputNode
     10:44:44.496  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=512 lastPeak=0.00470
     10:44:44.496  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=1, isUsbInputFxMode=0, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00470
     10:44:45.925  I  USB FEED: 256 frames, maxSample=0.0145, ringAvail=95744, monitorEnabled=0, monitorAvail=0
     10:44:45.925  I  USB MIX/VOCODER: Fed 256 frames to InputNode
     10:44:46.097  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=512 lastPeak=0.01068
     10:44:46.097  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=1, isUsbInputFxMode=0, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.01068
     10:44:47.696  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079cf309800 outputPtr=0xb4000079ce940000 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=512 lastPeak=0.00227
     10:44:47.696  I  USB_CB: inputData=0xb4000079cf309800, oscEnabled=1, isUsbInputFxMode=0, outputData=0xb4000079ce940000, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00227
     10:44:48.593  I  USB FEED: 256 frames, maxSample=0.0044, ringAvail=95744, monitorEnabled=0, monitorAvail=0
     10:44:48.593  I  USB MIX/VOCODER: Fed 256 frames to InputNode
     10:44:48.637  I  Stopping LibusbBackend...
     10:44:48.638  I  DSP thread stopped
     10:44:48.638  I  Stopping USB transfers...
     10:44:48.639  I  USB event loop stopped, disconnected=0
     10:44:48.654  I  Released interface 2
     10:44:48.670  I  Released interface 1
     10:44:48.670  I  Stopped USB transfers. Stats: submitted=46512, completed=92976, errors=0, underruns=0
     10:44:48.670  I  LibusbBackend stopped, deviceDisconnected=0

2.   UGREEN CM720 (UAC2, VID=0x2B89)
2.a Playback solo → audio sale limpio pasa
2.b Full-duplex si tiene capture → funciona: pasa 
2.b Situación parecida a la anterior, pero en ese caso el playback al principio sonaba mal y luego se arreglo al pasar al modo input fx, logs: 
10:48:25.310  D  UsbAudioManager created
10:48:25.310  I  Starting USB device monitoring
10:48:25.311  I  BroadcastReceiver registered (pre-Android 13)
10:48:25.311  I  Listening for actions: ATTACHED, DETACHED, com.watermellonstudios.audio.USB_PERMISSION
10:48:25.313  D  Auto-connect enabled
10:48:25.318  D  Found 0 USB Audio devices
10:48:30.322  D  USB device attached
10:48:30.322  D  BroadcastReceiver.onReceive: action=android.hardware.usb.action.USB_DEVICE_ATTACHED
10:48:30.323  I  USB Audio device attached: UGREEN CM720 USB Audio
10:48:30.323  I    VID=0x2B89, PID=0x64EC
10:48:30.323  I    Manufacturer: Realtek
10:48:30.323  I    Product: UGREEN CM720 USB Audio
10:48:30.323  I    Device ID: 1002
10:48:30.326  I  USB DEVICE: id=1002, VID=0x2B89, PID=0x64EC, product=UGREEN CM720 USB Audio, manufacturer=Realtek, interfaces=8
10:48:30.330  I  Compatibility check: DEBUG_ONLY - Device allowed in DEBUG build (VID:PID = 2B89:64EC)
10:48:30.331  I  RECONNECT: autoConnect=true, selected=null, state=DISCONNECTED, hasPermission=false
10:48:30.331  I  Compatible device detected, auto-connect: true
10:48:30.331  I    selectedDevice before auto-connect: null
10:48:30.331  I    isDeviceReady before auto-connect: false
10:48:30.331  I  Auto-connecting to Realtek UGREEN CM720 USB Audio
10:48:30.332  D  Connecting to device: Realtek UGREEN CM720 USB Audio
10:48:31.423  I  Device Realtek UGREEN CM720 USB Audio is in trusted list
10:48:31.424  I  Trusted device requires permission again. User should check 'Always use NoisyPad for this device'
10:48:31.424  I  Trusted device needs re-authorization: Realtek UGREEN CM720 USB Audio
10:48:31.425  I  requestPermissionSuspend: Requesting permission for UGREEN CM720 USB Audio
10:48:31.426  I    Action: com.watermellonstudios.audio.USB_PERMISSION
10:48:31.426  I    Package: com.watermellonstudios.noisypad
10:48:31.426  I    Flags: 167772160 (FLAG_MUTABLE=33554432, FLAG_UPDATE_CURRENT=134217728)
10:48:31.426  I    Device: UGREEN CM720 USB Audio (id=1002)
10:48:31.426  I    isMonitoring: true
10:48:31.441  I  Permission dialog should now appear - waiting for user response...
10:48:31.704  D    - USB-Audio - UGREEN CM720 USB Audio (USB_DAC)
10:48:31.704  D  Auto-selecting device: USB-Audio - UGREEN CM720 USB Audio (USB_DAC)
10:48:31.705  D  Auto-selected input source: USB_DAC
10:48:31.716  D  Input device added: USB-Audio - UGREEN CM720 USB Audio (type=22)
10:48:31.717  D    - USB-Audio - UGREEN CM720 USB Audio (USB_DAC)
10:48:31.717  D  Auto-selecting device: USB-Audio - UGREEN CM720 USB Audio (USB_DAC)
10:48:31.717  D  Auto-selected input source: USB_DAC
10:48:32.157  D  BroadcastReceiver.onReceive: action=com.watermellonstudios.audio.USB_PERMISSION
10:48:32.157  I  ACTION_USB_PERMISSION received!
10:48:32.158  I    Intent extras: device, permission
10:48:32.158  I    device: UGREEN CM720 USB Audio (id=1002)
10:48:32.158  I    granted: true
10:48:32.158  I    pendingPermissionDevice: UGREEN CM720 USB Audio (id=1002)
10:48:32.158  I    permissionContinuation is null: false
10:48:32.158  D  handlePermissionResult called: device=UGREEN CM720 USB Audio, granted=true
10:48:32.158  I  Permission result for UGREEN CM720 USB Audio: true
10:48:32.161  I  Added trusted device: Realtek UGREEN CM720 USB Audio (11145:25836)
10:48:32.161  I  Device Realtek UGREEN CM720 USB Audio added to trusted devices
10:48:32.163  I  Connected to Realtek UGREEN CM720 USB Audio, FD=105
10:48:32.163  D  Initialize native USB: fd=105, path=/dev/bus/usb/001/002
10:48:32.163  I  AudioNativeBridge.initializeUsbDevice: fd=105, path=/dev/bus/usb/001/002
10:48:32.163  I  Initializing USB backend: fd=105, path=/dev/bus/usb/001/002
10:48:32.163  I  LibusbBackend created
10:48:32.163  I  Initializing from fd=105, path=/dev/bus/usb/001/002
10:48:32.161  W  type=1400 audit(0.0:1193): avc: denied { read } for name="usb" dev="tmpfs" ino=467906 scontext=u:r:untrusted_app:s0:c76,c257,c512,c768 tcontext=u:object_r:usb_device:s0 tclass=dir permissive=0 app=com.watermellonstudios.noisypad
10:48:32.165  I  USB Device: VID=0x2B89, PID=0x64EC
10:48:32.166  I  Manufacturer: Realtek
10:48:32.168  I  Product: UGREEN CM720 USB Audio
10:48:32.169  I  Config descriptor: totalLength=446, configValue=1
10:48:32.214  I  Got raw config descriptor: 446 bytes
10:48:32.214  D  Configuration descriptor: 4 interfaces, total length 446
10:48:32.214  D  Found Audio Control interface: 0
10:48:32.214  D  UAC 2.0 AC Header: bcdADC=0x0200, bCategory=0x04
10:48:32.214  D  UAC 2.0 Clock Source: ID=27, Type=3, SyncToSOF=1, FreqCtrl=1
10:48:32.214  D  UAC 2.0 Clock Source: ID=30, Type=3, SyncToSOF=1, FreqCtrl=1
10:48:32.214  D  UAC 2.0 Input Terminal: ID=1, Type=0x0201, Channels=2, ClockSrc=27
10:48:32.214  D  UAC 2.0 Feature Unit: ID=3, SourceID=1, Channels=1, Mute=1, Volume=0
10:48:32.214  D    Channel 0 controls: 0x00000003 (Mute=1, Volume=0)
10:48:32.214  D    Channel 1 controls: 0x0000000c (Mute=0, Volume=1)
10:48:32.214  D  UAC 2.0 Output Terminal: ID=2, Type=0x0101, SourceID=4, ClockSrc=27
10:48:32.214  D  UAC 2.0 Input Terminal: ID=34, Type=0x0201, Channels=2, ClockSrc=27
10:48:32.214  D  UAC 2.0 Feature Unit: ID=35, SourceID=34, Channels=1, Mute=1, Volume=0
10:48:32.215  D    Channel 0 controls: 0x00000003 (Mute=1, Volume=0)
10:48:32.215  D    Channel 1 controls: 0x0000000c (Mute=0, Volume=1)
10:48:32.215  D  Skipping UAC 2.0 AC descriptor subtype: 0x04
10:48:32.215  D  Skipping UAC 2.0 AC descriptor subtype: 0x09
10:48:32.215  D  UAC 2.0 Input Terminal: ID=14, Type=0x0101, Channels=2, ClockSrc=30
10:48:32.215  D  UAC 2.0 Feature Unit: ID=16, SourceID=14, Channels=1, Mute=1, Volume=0
10:48:32.215  D    Channel 0 controls: 0x00000003 (Mute=1, Volume=0)
10:48:32.215  D    Channel 1 controls: 0x0000000c (Mute=0, Volume=1)
10:48:32.215  D  UAC 2.0 Output Terminal: ID=15, Type=0x0302, SourceID=36, ClockSrc=30
10:48:32.215  D  Found Audio Streaming interface: 1 alt 0 (endpoints: 0)
10:48:32.215  D  Found Audio Streaming interface: 1 alt 1 (endpoints: 1)
10:48:32.215  D  UAC 2.0 AS General: TerminalLink=2, FormatType=1, Channels=2, bmFormats=0x00000001
10:48:32.215  D  UAC 2.0 Format Type I: SubslotSize=2, BitRes=16
10:48:32.215  D  Endpoint: Address=0x81, Attr=0x05, MaxPacket=28, Interval=1
10:48:32.215  D    Isochronous endpoint: IN, Async
10:48:32.215  D  Audio Endpoint: bmAttr=0x00 (SamFreq=0, Pitch=0, MaxPkt=0)
10:48:32.215  D  Found Audio Streaming interface: 2 alt 0 (endpoints: 0)
10:48:32.215  D  Found Audio Streaming interface: 2 alt 1 (endpoints: 1)
10:48:32.215  D  UAC 2.0 AS General: TerminalLink=14, FormatType=1, Channels=2, bmFormats=0x00000001
10:48:32.215  D  UAC 2.0 Format Type I: SubslotSize=2, BitRes=16
10:48:32.215  D  Endpoint: Address=0x07, Attr=0x09, MaxPacket=248, Interval=1
10:48:32.215  D    Isochronous endpoint: OUT, Adaptive
10:48:32.215  D  Audio Endpoint: bmAttr=0x00 (SamFreq=0, Pitch=0, MaxPkt=0)
10:48:32.215  D  Found Audio Streaming interface: 2 alt 2 (endpoints: 1)
10:48:32.215  D  UAC 2.0 AS General: TerminalLink=14, FormatType=1, Channels=2, bmFormats=0x00000001
10:48:32.215  D  UAC 2.0 Format Type I: SubslotSize=3, BitRes=24
10:48:32.215  D  Endpoint: Address=0x07, Attr=0x09, MaxPacket=372, Interval=1
10:48:32.215  D    Isochronous endpoint: OUT, Adaptive
10:48:32.215  D  Audio Endpoint: bmAttr=0x00 (SamFreq=0, Pitch=0, MaxPkt=0)
10:48:32.215  D  Found Audio Streaming interface: 2 alt 3 (endpoints: 1)
10:48:32.215  D  UAC 2.0 AS General: TerminalLink=14, FormatType=1, Channels=2, bmFormats=0x00000001
10:48:32.215  D  UAC 2.0 Format Type I: SubslotSize=4, BitRes=32
10:48:32.215  D  Endpoint: Address=0x07, Attr=0x09, MaxPacket=496, Interval=1
10:48:32.215  D    Isochronous endpoint: OUT, Adaptive
10:48:32.215  D  Audio Endpoint: bmAttr=0x00 (SamFreq=0, Pitch=0, MaxPkt=0)
10:48:32.215  I  Successfully parsed USB Audio device: VID=2b89 PID=64ec
10:48:32.215  I    Playback interfaces: 3, Capture interfaces: 1
10:48:32.215  I  Parsed USB Audio device:
10:48:32.215  I    UAC Version: 2
10:48:32.215  I    Playback interfaces: 3
10:48:32.215  I    Capture interfaces: 1
10:48:32.215  I  selectBestInterfaces: mode=PLAYBACK_ONLY, needsPlayback=1, needsCapture=0
10:48:32.215  I  Available playback altsettings (3):
10:48:32.215  I    [0] IF2 Alt1: 2ch/16bit (1 fmt) rates={unknown} ep=0x07 fb=no
10:48:32.215  I    [1] IF2 Alt2: 2ch/24bit (1 fmt) rates={unknown} ep=0x07 fb=no
10:48:32.215  I    [2] IF2 Alt3: 2ch/32bit (1 fmt) rates={unknown} ep=0x07 fb=no
10:48:32.215  I  Selected playback: IF2 Alt3, 48000Hz, 2ch, 32bit, score=1.571
10:48:32.219  I  Detached kernel driver from control interface 0
10:48:32.219  I  Claimed AudioControl interface 0
10:48:32.219  I  UsbVolumeControl: Initializing FU=3, hasVolume=1, hasMute=1, isOutput=1, UAC=2
10:48:32.220  D  UsbVolumeControl: UAC 2.0 RANGE returned 1 ranges, using first
10:48:32.220  I  UsbVolumeControl: Volume range: min=-4512 (-17.6 dB), max=0 (0.0 dB), res=96
10:48:32.220  I  UsbVolumeControl: Hardware volume available, current=0 (0.0 dB)
10:48:32.221  I  UsbVolumeControl: Hardware mute available, current=0
10:48:32.221  I  UsbVolumeControl: Initialized - HW volume=1, HW mute=1, range=[-4512, 0] res=96
10:48:32.221  I  Output volume control: HW=1, Volume=1, Mute=1, FU=3
10:48:32.221  I  USB device initialized successfully
10:48:32.221  I  USB backend initialized successfully
10:48:32.221  I  AudioNativeBridge.initializeUsbDevice: LibusbBackend initialized successfully
10:48:32.221  I  Native USB device initialized successfully
10:48:32.221  D  USB Capabilities parsed: [3.0, 1.0, 48000.0, 7.0, 2.0, 2.0, 2.0]
10:48:32.222  I  Auto-connect result: Success(value=kotlin.Unit)
10:48:32.222  I    isDeviceReady after auto-connect: true
10:48:32.222  I  Auto-connect successful, emitting CompatibleDeviceDetected
10:48:32.222  I  Compatible USB device detected: Realtek UGREEN CM720 USB Audio
10:48:32.222  I  Device ready immediately, auto-switching to USB backend
10:48:32.223  I  Backend selected: USB Audio
10:48:32.223  I  Switched to USB Direct backend
10:48:32.223  I  Emitting DeviceConnected event
10:48:32.223  I  Auto-switched to USB backend successfully
10:48:32.224  D  USB capabilities updated: hasCapture=true, supportsFullDuplex=true
10:48:32.638  I  App launched/resumed via USB_DEVICE_ATTACHED intent
10:48:32.638  I    Device: UGREEN CM720 USB Audio (VID=11145, PID=25836)
10:48:32.639  I  handleUsbDeviceFromIntent: deviceId=1002
10:48:32.644  I  USB DEVICE: id=1002, VID=0x2B89, PID=0x64EC, product=UGREEN CM720 USB Audio, manufacturer=Realtek, interfaces=8
10:48:32.644  D  Found 1 USB Audio devices
10:48:32.746  I  Found device from intent: Realtek UGREEN CM720 USB Audio
10:48:33.564  D  TogglePlay: Using LIBUSB backend
10:48:33.567  I  selectBestInterfaces: mode=PLAYBACK_ONLY, needsPlayback=1, needsCapture=0
10:48:33.567  I  Available playback altsettings (3):
10:48:33.567  I    [0] IF2 Alt1: 2ch/16bit (1 fmt) rates={unknown} ep=0x07 fb=no
10:48:33.567  I    [1] IF2 Alt2: 2ch/24bit (1 fmt) rates={unknown} ep=0x07 fb=no
10:48:33.567  I    [2] IF2 Alt3: 2ch/32bit (1 fmt) rates={unknown} ep=0x07 fb=no
10:48:33.567  I  Selected playback: IF2 Alt3, 48000Hz, 2ch, 32bit, score=1.571
10:48:33.567  I  Starting with full-duplex=false, capture selected=no
10:48:33.567  I  UsbTransferManager UAC version set to 2
10:48:33.567  I  USB speed: HIGH (libusb=3) → 8 packets/ms, 6 frames/packet, 64 packets/xfer
10:48:33.567  I  Ring buffer config: ringBufferMs=100, outputSize=9600 samples (38400 bytes), inputSize=9600 samples (38400 bytes)
10:48:33.568  I  Configured: 48000Hz, out=2ch/32bit, in=2ch/32bit, 6 frames/packet, 64 packets/xfer
10:48:33.568  I  Output interface set: IF2 Alt3, EP 0x07
10:48:33.569  I  Detached kernel driver from interface 2
10:48:33.569  I  Claimed interface 2
10:48:33.645  I  Set interface 2 to alt setting 3
10:48:33.646  I  Rate negotiation: UAC2 clockSrc=27 req=48000 actual=48000
10:48:33.646  I  Allocating output transfers: nominal=48, clockMargin=+32, endpoint wMaxPacketSize=0x01f0 (effective=496, informational), buffer stride=80 bytes/packet
10:48:33.647  I  Started USB transfers
10:48:33.647  I  USB event loop started
10:48:33.648  I  Pre-allocated DSP buffers: frames=256, outCh=2(512 samples), inCh=0(256 samples), monoToStereo=0
10:48:33.648  I  LibusbBackend started
10:48:33.648  I  Backend started: USB Audio
10:48:33.648  I    Backend type: USB Audio
10:48:33.648  I  DSP thread started, mode=PLAYBACK_ONLY
10:48:33.648  I  Event loop thread pinned to core 7 (of 8)
10:48:33.648  I  DSP thread pinned to core 6 (of 8)
10:48:34.493  I  START_USB_FADE: sampleRate=48000, fadeTimeMs=500
10:48:36.024  I  USB_DSP: hasCapture=0 inputPtr=0x0 outputPtr=0xb4000079daadd800 frames=256 streamMode=0 | read ok=0 fail=0 ratio=0.00 ringAvailPre=0 lastPeak=0.00000
10:48:36.025  I  USB_CB: inputData=0x0, oscEnabled=1, isUsbInputFxMode=0, outputData=0xb4000079daadd800, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00000
10:48:38.424  I  USB_DSP: hasCapture=0 inputPtr=0x0 outputPtr=0xb4000079daadd800 frames=256 streamMode=0 | read ok=0 fail=0 ratio=0.00 ringAvailPre=0 lastPeak=0.00000
10:48:38.424  I  USB_CB: inputData=0x0, oscEnabled=1, isUsbInputFxMode=0, outputData=0xb4000079daadd800, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00000
10:48:40.817  I  USB_DSP: hasCapture=0 inputPtr=0x0 outputPtr=0xb4000079daadd800 frames=256 streamMode=0 | read ok=0 fail=0 ratio=0.00 ringAvailPre=0 lastPeak=0.00000
10:48:40.817  I  USB_CB: inputData=0x0, oscEnabled=1, isUsbInputFxMode=0, outputData=0xb4000079daadd800, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00000
10:48:43.177  I  USB_DSP: hasCapture=0 inputPtr=0x0 outputPtr=0xb4000079daadd800 frames=256 streamMode=0 | read ok=0 fail=0 ratio=0.00 ringAvailPre=0 lastPeak=0.00000
10:48:43.177  I  USB_CB: inputData=0x0, oscEnabled=1, isUsbInputFxMode=0, outputData=0xb4000079daadd800, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00000
10:48:45.569  I  USB_DSP: hasCapture=0 inputPtr=0x0 outputPtr=0xb4000079daadd800 frames=256 streamMode=0 | read ok=0 fail=0 ratio=0.00 ringAvailPre=0 lastPeak=0.00000
10:48:45.569  I  USB_CB: inputData=0x0, oscEnabled=1, isUsbInputFxMode=0, outputData=0xb4000079daadd800, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00000
10:48:47.968  I  USB_DSP: hasCapture=0 inputPtr=0x0 outputPtr=0xb4000079daadd800 frames=256 streamMode=0 | read ok=0 fail=0 ratio=0.00 ringAvailPre=0 lastPeak=0.00000
10:48:47.968  I  USB_CB: inputData=0x0, oscEnabled=1, isUsbInputFxMode=0, outputData=0xb4000079daadd800, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00000
10:48:50.344  I  USB_DSP: hasCapture=0 inputPtr=0x0 outputPtr=0xb4000079daadd800 frames=256 streamMode=0 | read ok=0 fail=0 ratio=0.00 ringAvailPre=0 lastPeak=0.00000
10:48:50.344  I  USB_CB: inputData=0x0, oscEnabled=1, isUsbInputFxMode=0, outputData=0xb4000079daadd800, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00000
10:48:52.190  I  prepareForInputMode: currentBackend=LIBUSB, usbAvailable=true
10:48:52.190  I  prepareForInputMode: USB active, ensuring FULL_DUPLEX
10:48:52.192  I  Stopping LibusbBackend...
10:48:52.193  I  DSP thread stopped
10:48:52.193  I  Stopping USB transfers...
10:48:52.195  I  USB event loop stopped, disconnected=0
10:48:52.207  I  Released interface 2
10:48:52.207  I  Stopped USB transfers. Stats: submitted=148544, completed=148352, errors=0, underruns=761
10:48:52.207  I  LibusbBackend stopped, deviceDisconnected=0
10:48:52.412  I  selectBestInterfaces: mode=FULL_DUPLEX, needsPlayback=1, needsCapture=1
10:48:52.412  I  Available playback altsettings (3):
10:48:52.412  I    [0] IF2 Alt1: 2ch/16bit (1 fmt) rates={unknown} ep=0x07 fb=no
10:48:52.412  I    [1] IF2 Alt2: 2ch/24bit (1 fmt) rates={unknown} ep=0x07 fb=no
10:48:52.412  I    [2] IF2 Alt3: 2ch/32bit (1 fmt) rates={unknown} ep=0x07 fb=no
10:48:52.412  I  Selected playback: IF2 Alt3, 48000Hz, 2ch, 32bit, score=1.571
10:48:52.412  I  Available capture altsettings (1):
10:48:52.412  I    [0] IF1 Alt1: 2ch/16bit (1 fmt) rates={unknown} ep=0x81 fb=no
10:48:52.412  I  Selected capture: IF1 Alt1, 48000Hz, 2ch, 16bit, score=1.071
10:48:52.412  I  Starting with full-duplex=true, capture selected=yes
10:48:52.412  I  Tearing down previous transfer manager before restart
10:48:52.413  I  UsbTransferManager UAC version set to 2
10:48:52.413  I  Input config: 2 channels, 16-bit (output: 2 channels, 32-bit)
10:48:52.413  I  USB speed: HIGH (libusb=3) → 8 packets/ms, 6 frames/packet, 64 packets/xfer
10:48:52.413  I  Ring buffer config: ringBufferMs=100, outputSize=9600 samples (38400 bytes), inputSize=9600 samples (38400 bytes)
10:48:52.414  I  Configured: 48000Hz, out=2ch/32bit, in=2ch/16bit, 6 frames/packet, 64 packets/xfer
10:48:52.414  I  Output interface set: IF2 Alt3, EP 0x07
10:48:52.414  I  Input interface set: IF1 Alt1, EP 0x81
10:48:52.415  I  Claimed interface 2
10:48:52.493  I  Set interface 2 to alt setting 3
10:48:52.494  I  Detached kernel driver from interface 1
10:48:52.495  I  Claimed interface 1
10:48:52.504  I  Set interface 1 to alt setting 1
10:48:52.506  I  Rate negotiation: UAC2 clockSrc=27 req=48000 actual=48000
10:48:52.506  I  Allocating output transfers: nominal=48, clockMargin=+32, endpoint wMaxPacketSize=0x01f0 (effective=496, informational), buffer stride=80 bytes/packet
10:48:52.506  I  Allocating input transfers:  nominal=24, endpoint wMaxPacketSize=0x001c (effective=28), stride=28 bytes/packet (endpoint reserves headroom above nominal)
10:48:52.507  I  Allocating input transfers: size=1792 bytes (64 packets × 28 bytes/packet)
10:48:52.508  I  Started USB transfers
10:48:52.508  I  USB event loop started
10:48:52.508  I  Pre-allocated DSP buffers: frames=256, outCh=2(512 samples), inCh=2(512 samples), monoToStereo=0
10:48:52.508  I  LibusbBackend started
10:48:52.508  I  Backend started: USB Audio
10:48:52.508  I    Backend type: USB Audio
10:48:52.508  I  Event loop thread pinned to core 7 (of 8)
10:48:52.509  I  DSP thread started, mode=FULL_DUPLEX
10:48:52.509  I  DSP thread pinned to core 6 (of 8)
10:48:52.885  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079ca1e4000 outputPtr=0xb4000079daadd800 frames=256 streamMode=2 | read ok=69 fail=0 ratio=1.00 ringAvailPre=1198 lastPeak=0.00116
10:48:52.885  I  USB_CB: inputData=0xb4000079ca1e4000, oscEnabled=1, isUsbInputFxMode=0, outputData=0xb4000079daadd800, numFrames=256, state=2, paused=0, fadeVol=0.000, masterVol=1.000, inputPeak=0.00116
10:48:53.319  I  START_USB_FADE: sampleRate=48000, fadeTimeMs=200
10:48:53.646  I  SET_MODE_INPUT_FX: inputNode=0xb400007b228dee58, backendType=2, isUsbActive=1
10:48:53.646  I  AudioNativeBridge.setAudioMode: USB active, stopping Oboe input
10:48:54.484  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079ca1e4000 outputPtr=0xb4000079daadd800 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=920 lastPeak=0.00732
10:48:54.484  I  USB_CB: inputData=0xb4000079ca1e4000, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079daadd800, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00732
10:48:55.241  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.01066, effects=2
10:48:56.089  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079ca1e4000 outputPtr=0xb4000079daadd800 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=620 lastPeak=0.00113
10:48:56.089  I  USB_CB: inputData=0xb4000079ca1e4000, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079daadd800, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00113
10:48:56.309  I  USB DIRECT INPUT_FX: Processing 256 frames directly (no InputNode buffering)
10:48:56.848  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.01314, effects=2
10:48:57.694  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079ca1e4000 outputPtr=0xb4000079daadd800 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=1064 lastPeak=0.00095
10:48:57.694  I  USB_CB: inputData=0xb4000079ca1e4000, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079daadd800, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00089
10:48:58.450  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.00711, effects=2
10:48:59.297  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079ca1e4000 outputPtr=0xb4000079daadd800 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=718 lastPeak=0.00122
10:48:59.297  I  USB_CB: inputData=0xb4000079ca1e4000, oscEnabled=1, isUsbInputFxMode=0, outputData=0xb4000079daadd800, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00122
10:49:00.900  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079ca1e4000 outputPtr=0xb4000079daadd800 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=1112 lastPeak=0.00082
10:49:00.900  I  USB_CB: inputData=0xb4000079ca1e4000, oscEnabled=1, isUsbInputFxMode=0, outputData=0xb4000079daadd800, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00073
10:49:01.485  I  USB FEED: 256 frames, maxSample=0.0036, ringAvail=95744, monitorEnabled=0, monitorAvail=0
10:49:01.485  I  USB MIX/VOCODER: Fed 256 frames to InputNode
10:49:02.505  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079ca1e4000 outputPtr=0xb4000079daadd800 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=720 lastPeak=0.00168
10:49:02.505  I  USB_CB: inputData=0xb4000079ca1e4000, oscEnabled=1, isUsbInputFxMode=0, outputData=0xb4000079daadd800, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00143
10:49:03.052  I  prepareForInputMode: currentBackend=LIBUSB, usbAvailable=true
10:49:03.052  I  prepareForInputMode: USB active, ensuring FULL_DUPLEX
10:49:03.053  I  Stopping LibusbBackend...
10:49:03.054  I  DSP thread stopped
10:49:03.054  I  Stopping USB transfers...
10:49:03.056  I  USB event loop stopped, disconnected=0
10:49:03.063  I  Released interface 2
10:49:03.087  I  Released interface 1
10:49:03.087  I  Stopped USB transfers. Stats: submitted=84544, completed=168704, errors=0, underruns=2
10:49:03.087  I  LibusbBackend stopped, deviceDisconnected=0
10:49:03.301  I  selectBestInterfaces: mode=FULL_DUPLEX, needsPlayback=1, needsCapture=1
10:49:03.301  I  Available playback altsettings (3):
10:49:03.301  I    [0] IF2 Alt1: 2ch/16bit (1 fmt) rates={unknown} ep=0x07 fb=no
10:49:03.301  I    [1] IF2 Alt2: 2ch/24bit (1 fmt) rates={unknown} ep=0x07 fb=no
10:49:03.302  I    [2] IF2 Alt3: 2ch/32bit (1 fmt) rates={unknown} ep=0x07 fb=no
10:49:03.302  I  Selected playback: IF2 Alt3, 48000Hz, 2ch, 32bit, score=1.571
10:49:03.302  I  Available capture altsettings (1):
10:49:03.302  I    [0] IF1 Alt1: 2ch/16bit (1 fmt) rates={unknown} ep=0x81 fb=no
10:49:03.302  I  Selected capture: IF1 Alt1, 48000Hz, 2ch, 16bit, score=1.071
10:49:03.302  I  Starting with full-duplex=true, capture selected=yes
10:49:03.302  I  Tearing down previous transfer manager before restart
10:49:03.303  I  UsbTransferManager UAC version set to 2
10:49:03.303  I  Input config: 2 channels, 16-bit (output: 2 channels, 32-bit)
10:49:03.303  I  USB speed: HIGH (libusb=3) → 8 packets/ms, 6 frames/packet, 64 packets/xfer
10:49:03.303  I  Ring buffer config: ringBufferMs=100, outputSize=9600 samples (38400 bytes), inputSize=9600 samples (38400 bytes)
10:49:03.304  I  Configured: 48000Hz, out=2ch/32bit, in=2ch/16bit, 6 frames/packet, 64 packets/xfer
10:49:03.304  I  Output interface set: IF2 Alt3, EP 0x07
10:49:03.304  I  Input interface set: IF1 Alt1, EP 0x81
10:49:03.304  I  Claimed interface 2
10:49:03.389  I  Set interface 2 to alt setting 3
10:49:03.389  I  Claimed interface 1
10:49:03.400  I  Set interface 1 to alt setting 1
10:49:03.402  I  Rate negotiation: UAC2 clockSrc=27 req=48000 actual=48000
10:49:03.402  I  Allocating output transfers: nominal=48, clockMargin=+32, endpoint wMaxPacketSize=0x01f0 (effective=496, informational), buffer stride=80 bytes/packet
10:49:03.402  I  Allocating input transfers:  nominal=24, endpoint wMaxPacketSize=0x001c (effective=28), stride=28 bytes/packet (endpoint reserves headroom above nominal)
10:49:03.403  I  Allocating input transfers: size=1792 bytes (64 packets × 28 bytes/packet)
10:49:03.404  I  Started USB transfers
10:49:03.404  I  USB event loop started
10:49:03.404  I  Pre-allocated DSP buffers: frames=256, outCh=2(512 samples), inCh=2(512 samples), monoToStereo=0
10:49:03.404  I  LibusbBackend started
10:49:03.404  I  Backend started: USB Audio
10:49:03.404  I    Backend type: USB Audio
10:49:03.405  I  Event loop thread pinned to core 7 (of 8)
10:49:03.405  I  DSP thread started, mode=FULL_DUPLEX
10:49:03.405  I  DSP thread pinned to core 6 (of 8)
10:49:04.220  I  START_USB_FADE: sampleRate=48000, fadeTimeMs=200
10:49:04.468  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079ca1e4000 outputPtr=0xb4000079daadd800 frames=256 streamMode=2 | read ok=198 fail=0 ratio=1.00 ringAvailPre=982 lastPeak=0.00177
10:49:04.468  I  USB_CB: inputData=0xb4000079ca1e4000, oscEnabled=1, isUsbInputFxMode=0, outputData=0xb4000079daadd800, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00177
10:49:04.518  I  USB FEED: 256 frames, maxSample=0.0070, ringAvail=95744, monitorEnabled=0, monitorAvail=0
10:49:04.518  I  USB MIX/VOCODER: Fed 256 frames to InputNode
10:49:04.529  I  SET_MODE_INPUT_FX: inputNode=0xb400007b228dee58, backendType=2, isUsbActive=1
10:49:04.529  I  AudioNativeBridge.setAudioMode: USB active, stopping Oboe input
10:49:04.700  I  USB DIRECT INPUT_FX: Processing 256 frames directly (no InputNode buffering)
10:49:05.771  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.00407, effects=2
10:49:06.074  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079ca1e4000 outputPtr=0xb4000079daadd800 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=542 lastPeak=0.00510
10:49:06.074  I  USB_CB: inputData=0xb4000079ca1e4000, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079daadd800, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00510
10:49:07.373  I  USB DIRECT INPUT_FX: Processing 256 frames directly (no InputNode buffering)
10:49:07.377  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.01222, effects=2
10:49:07.677  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079ca1e4000 outputPtr=0xb4000079daadd800 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=844 lastPeak=0.07336
10:49:07.677  I  USB_CB: inputData=0xb4000079ca1e4000, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079daadd800, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.04733
10:49:08.985  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.00824, effects=2
10:49:09.284  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079ca1e4000 outputPtr=0xb4000079daadd800 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=1124 lastPeak=0.00107
10:49:09.284  I  USB_CB: inputData=0xb4000079ca1e4000, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079daadd800, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00064
10:49:10.048  I  USB DIRECT INPUT_FX: Processing 256 frames directly (no InputNode buffering)
10:49:10.590  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.00496, effects=2
10:49:10.888  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079ca1e4000 outputPtr=0xb4000079daadd800 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=614 lastPeak=0.00137
10:49:10.888  I  USB_CB: inputData=0xb4000079ca1e4000, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079daadd800, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00089
10:49:12.194  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.00100, effects=3
10:49:12.496  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079ca1e4000 outputPtr=0xb4000079daadd800 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=846 lastPeak=0.00226
10:49:12.496  I  USB_CB: inputData=0xb4000079ca1e4000, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079daadd800, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00226
10:49:12.724  I  USB DIRECT INPUT_FX: Processing 256 frames directly (no InputNode buffering)
10:49:13.801  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.00258, effects=3
10:49:14.102  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079ca1e4000 outputPtr=0xb4000079daadd800 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=1054 lastPeak=0.05170
10:49:14.102  I  USB_CB: inputData=0xb4000079ca1e4000, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079daadd800, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.03461
10:49:15.401  I  USB DIRECT INPUT_FX: Processing 256 frames directly (no InputNode buffering)
10:49:15.405  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.00199, effects=3
10:49:15.709  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079ca1e4000 outputPtr=0xb4000079daadd800 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=1238 lastPeak=0.00119
10:49:15.709  I  USB_CB: inputData=0xb4000079ca1e4000, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079daadd800, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00119
10:49:17.014  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.00567, effects=3
10:49:17.313  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079ca1e4000 outputPtr=0xb4000079daadd800 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=634 lastPeak=0.00165
10:49:17.313  I  USB_CB: inputData=0xb4000079ca1e4000, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079daadd800, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00165
10:49:18.082  I  USB DIRECT INPUT_FX: Processing 256 frames directly (no InputNode buffering)
10:49:18.618  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.00158, effects=3
10:49:18.916  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079ca1e4000 outputPtr=0xb4000079daadd800 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=772 lastPeak=0.00208
10:49:18.916  I  USB_CB: inputData=0xb4000079ca1e4000, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079daadd800, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00153
10:49:20.226  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.00303, effects=3
10:49:20.525  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079ca1e4000 outputPtr=0xb4000079daadd800 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=886 lastPeak=0.00177
10:49:20.525  I  USB_CB: inputData=0xb4000079ca1e4000, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079daadd800, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00177
10:49:20.757  I  USB DIRECT INPUT_FX: Processing 256 frames directly (no InputNode buffering)
10:49:21.834  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.00471, effects=3
10:49:22.136  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079ca1e4000 outputPtr=0xb4000079daadd800 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=976 lastPeak=0.00095
10:49:22.136  I  USB_CB: inputData=0xb4000079ca1e4000, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079daadd800, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00082
10:49:23.436  I  USB DIRECT INPUT_FX: Processing 256 frames directly (no InputNode buffering)
10:49:23.440  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.00144, effects=3
10:49:23.740  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079ca1e4000 outputPtr=0xb4000079daadd800 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=1042 lastPeak=0.00204
10:49:23.740  I  USB_CB: inputData=0xb4000079ca1e4000, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079daadd800, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00092
10:49:25.049  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.00402, effects=3
10:49:25.348  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079ca1e4000 outputPtr=0xb4000079daadd800 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=1086 lastPeak=0.00204
10:49:25.348  I  USB_CB: inputData=0xb4000079ca1e4000, oscEnabled=0, isUsbInputFxMode=1, outputData=0xb4000079daadd800, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00204
10:49:26.117  I  USB DIRECT INPUT_FX: Processing 256 frames directly (no InputNode buffering)
10:49:26.658  I  USB_DIRECT_OUT: gainStart=1.000, gainEnd=1.000, fadeStart=1.000, fadeEnd=1.000, masterVol=1.000, outPeak=0.00092, effects=3
10:49:26.957  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079ca1e4000 outputPtr=0xb4000079daadd800 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=1104 lastPeak=0.00278
10:49:26.957  I  USB_CB: inputData=0xb4000079ca1e4000, oscEnabled=1, isUsbInputFxMode=0, outputData=0xb4000079daadd800, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00070
10:49:28.565  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079ca1e4000 outputPtr=0xb4000079daadd800 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=1100 lastPeak=0.00150
10:49:28.565  I  USB_CB: inputData=0xb4000079ca1e4000, oscEnabled=1, isUsbInputFxMode=0, outputData=0xb4000079daadd800, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00101
10:49:29.604  I  USB FEED: 256 frames, maxSample=0.0068, ringAvail=95744, monitorEnabled=0, monitorAvail=0
10:49:29.604  I  USB MIX/VOCODER: Fed 256 frames to InputNode
10:49:30.173  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079ca1e4000 outputPtr=0xb4000079daadd800 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=1072 lastPeak=0.00894
10:49:30.173  I  USB_CB: inputData=0xb4000079ca1e4000, oscEnabled=1, isUsbInputFxMode=0, outputData=0xb4000079daadd800, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00894
10:49:31.791  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079ca1e4000 outputPtr=0xb4000079daadd800 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=1784 lastPeak=0.00232
10:49:31.791  I  USB_CB: inputData=0xb4000079ca1e4000, oscEnabled=1, isUsbInputFxMode=0, outputData=0xb4000079daadd800, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00232
10:49:32.287  I  USB FEED: 256 frames, maxSample=0.0181, ringAvail=95744, monitorEnabled=0, monitorAvail=0
10:49:32.287  I  USB MIX/VOCODER: Fed 256 frames to InputNode
10:49:33.391  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079ca1e4000 outputPtr=0xb4000079daadd800 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=944 lastPeak=0.00262
10:49:33.391  I  USB_CB: inputData=0xb4000079ca1e4000, oscEnabled=1, isUsbInputFxMode=0, outputData=0xb4000079daadd800, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00262
10:49:34.965  I  USB FEED: 256 frames, maxSample=0.0078, ringAvail=95744, monitorEnabled=0, monitorAvail=0
10:49:34.965  I  USB MIX/VOCODER: Fed 256 frames to InputNode
10:49:34.998  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079ca1e4000 outputPtr=0xb4000079daadd800 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=844 lastPeak=0.00201
10:49:34.998  I  USB_CB: inputData=0xb4000079ca1e4000, oscEnabled=1, isUsbInputFxMode=0, outputData=0xb4000079daadd800, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00201
10:49:36.609  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079ca1e4000 outputPtr=0xb4000079daadd800 frames=256 streamMode=2 | read ok=300 fail=0 ratio=1.00 ringAvailPre=722 lastPeak=0.00128
10:49:36.609  I  USB_CB: inputData=0xb4000079ca1e4000, oscEnabled=1, isUsbInputFxMode=0, outputData=0xb4000079daadd800, numFrames=256, state=2, paused=0, fadeVol=1.000, masterVol=1.000, inputPeak=0.00037
10:49:37.649  I  USB FEED: 256 frames, maxSample=0.0106, ringAvail=95744, monitorEnabled=0, monitorAvail=0
10:49:37.649  I  USB MIX/VOCODER: Fed 256 frames to InputNode
10:49:38.217  I  USB_DSP: hasCapture=1 inputPtr=0xb4000079ca1e4000 outputPtr=0xb4000079daadd800 frames=256 stre
3. Con UC02 se replica el comportamiento de UGREEN CM720