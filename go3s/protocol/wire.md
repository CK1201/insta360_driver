# GO 3S bridge protocol v1

## Transport and framing

The phone accepts one TCP client at `127.0.0.1:18765`. The host creates an ADB forward from its configurable local `bridge.port` (default18765) to that phone port. No external network listener is created. Only one desktop client is active at a time; after it finishes, the listener accepts later explicitly started clients using the same camera control connection. Each client has a new sequence starting at0.

Each message consists of two unsigned32-bit big-endian lengths (`header_bytes`, `payload_bytes`), followed by UTF-8 JSON and the exact binary payload. Header size is at most1MiB, payload at most32MiB. All headers contain `v:1` and a string `type`. Every phone-to-host message additionally has `seq`, starting at0 and increasing by exactly1. Host commands are not sequenced.

## Host commands

- `start`: contains `camera` (serial, exposure, white_balance_kelvin), `stream.imu` and `runtime.status_interval_s` (0.1–60seconds). The shape is generated from the host YAML; only one start is accepted per connection.
- `stop`: requests preview shutdown and draining of already accepted events; it does not disconnect the phone from the camera.

The camera uses ordinary video preview. Neither command starts SD recording, RTMP streaming, arbitrary commands or FPS scanning.

## Phone events

| Type | JSON fields beyond v/type/seq | Payload |
|---|---|---|
| hello | phone model/android_sdk/abis, sdk_version, sdk_internal_loss_observable:false | empty |
| session | identity, requested, readback, preview codec/width/height/fps, sdk_internal_loss_observable:false | empty |
| video | camera_timestamp, phone_receive_ns, stream_index:0, codec:h264/h265 | original SDK callback bytes |
| gyro | camera_timestamp, phone_receive_ns, samples:[[device_timestamp,ax,ay,az,gx,gy,gz],...] | original SDK GYRO bytes |
| exposure | camera_timestamp, phone_receive_ns, exposure_time_s | empty |
| status | phone_receive_ns, connected, battery_level/scale, power, storage, preview | empty |
| error | message | empty |
| end | reason:stop/disconnect/error, sdk_internal_loss_observable:false | empty |

`session` is emitted after strict parameter readback and before starting preview. `preview` records the initial SDK choice; later `status.preview` and the decoded video dimensions record actual updates. `readback.exposure` contains mode/iso/shutter_s/ev; automatic ISO/shutter values remain automatic indicators.

GYRO batches are parsed with `OneGyroField(raw,offset,0)`: timestamp uses originTsMs and the six numeric values use accels/gyros. Values are not converted or deduplicated. If a GYRO batch cannot be parsed, its raw bytes are still delivered with an empty samples array and parse_error; the host must fail the capture even when end.reason is stop or earlier IMU samples were valid. Battery/storage retain SDK fields; unknown values remain unknown.

## Shutdown, integrity and clocks

The phone stops preview intake and drains accepted events before terminal error/end when the desktop connection remains writable. Normal stop or desktop EOF closes only that desktop session; the CameraDevice control connection, SDK heartbeat and foreground service remain alive. Only an explicit phone UI disconnect, service destruction or actual camera loss releases the camera. The host waits at most5seconds after stop; EOF without end is incomplete. Queues are bounded; overflow, sequence gaps and persistence errors fail the session. There is no automatic reconnect.

Camera raw timestamps, phone elapsedRealtimeNanos and host steady_clock are distinct domains. Sequence numbers prove ordering only from the application's SDK callbacks onward. The SDK's private128-event dispatch queue is not fully observable, so SDK-internal loss and clock synchronization are explicitly unverified.
