# Batch 2D texture export

In Event Browser, select draw/dispatch events with Ctrl/Shift or select a marker
group, then choose **Batch export textures...**. A group includes its nested draws
and dispatches. Event IDs and inclusive ranges such as `12, 40-80` can also be
entered; non-draw/non-dispatch events are ignored, overlaps are deduplicated.
This feature does not require the Autodesk FBX SDK.

1. Select the binding scope. PS input textures are enabled by default. Enable
   **Other shader stages** for VS/HS/DS/GS/CS/task/mesh inputs; compute dispatches
   need this option. Read-write textures are optional and follow the selected
   shader stages. Color/depth attachments can be included independently.
2. Choose DDS (default), PNG, or EXR, then click **Preflight**. The table groups
   bindings by resource and view component type. Select a row to inspect its
   events, stages, shader binding names/numbers, descriptor locations and view
   mip ranges. Uncheck textures that should not be exported. Changing the scope,
   event range, or format requires preflight again.
3. Click **Save**, then select an output directory. Existing files are skipped
   unless **Overwrite existing texture files** is enabled.

Only single-sample, non-array 2D textures are supported. 1D, 3D, arrays, Cube and
MSAA resources are listed as unsupported and cannot be selected. Only resource
**mip 0** is exported, even when the binding view starts at another mip; the view
range remains in the report. There are no mip, slice or image-flip controls.

## Data and file formats

The exporter uses the replay controller's texture-save API. It applies no image
flip, channel extraction, normal-map reconstruction, green-channel inversion,
or Texture Viewer display adjustments. DDS is recommended for preserving the
resource format and compressed data. Typeless resources use the binding view's
component type where supported. Descriptor swizzles and material/shader math are
not baked into the texture.

PNG explicitly converts to an 8-bit image and can clamp/quantize values outside
its representable range. EXR explicitly converts to a floating-point image using
RenderDoc's existing save path. These are format conversions, not byte-preserving
alternatives to DDS; no automatic format fallback occurs on failure. ETC/EAC/ASTC
cannot be preserved by DDS and are rejected for that format during preflight;
explicit PNG/EXR export is allowed. Other save-API errors are reported per file.
Depth/stencil resources follow the existing save API's format/channel handling;
separate stencil extraction is not provided in this version.

All data is captured **after the selected event**, including UAVs and output
attachments. For resources modified by that event this is not a snapshot of
their pre-event contents. The selected replay event is restored on completion,
failure or cancellation; the UI's selected event is not changed.

Bindings are obtained from the common pipeline-state descriptor API. A shader
reference does not guarantee runtime sampling. Provably unused references are
labelled; dynamically indexed/bindless resources are limited to the descriptors
the replay API reports. The tool does not scan every descriptor heap or infer
material roles from names.

## Event snapshots and deduplication

By default, the exporter reads each resource/view type at each selected event,
hashes a lossless mip-0 DDS with SHA-256, and reuses a file only when its contents
match another successfully exported snapshot of that same resource/view type.
Thus changing a resource between draws produces separate files. This avoids
duplicate output files but still requires replay and readback at each event.
Formats that cannot be preserved in DDS are not content-deduplicated, even when
explicitly converted to PNG/EXR.

**Export each resource only at its first selected event** skips later readbacks
and points their references at the first exported snapshot, regardless of later
changes. Use it only for known static assets. If the first snapshot fails or its
destination already exists and overwrite is disabled, later events do not
silently substitute another snapshot. Existing files are never assumed to match
the capture or used as verified deduplication targets.

Outputs use sanitized resource names, resource IDs, event IDs and view component
types. Each run writes a uniquely named `texture_export_report_<uuid>.json`
containing capture name, format, mip, snapshot timing, original event, results,
binding references, output paths, omissions and failure reasons. References can
be joined to the FBX batch report using event IDs; automatic material/FBX
association is not implemented.

Textures are processed sequentially and published through temporary files and
atomic replacement. Cancel stops subsequent exports after the current replay or
save call returns. Completed files remain; the report includes cancelled entries.
An individual failure does not stop other textures.

## Verification

Unit tests use `[texture-batch]`. Integration tests use
`[texture-batch-capture]` and require `RENDERDOC_TEXTURE_TEST_CAPTURE` to point to
the capture produced by `Tests/texture_batch_fixture.cpp`.

From an x64 Visual Studio developer command prompt at the repository root:

```bat
cl /nologo /EHsc /MD /I . qrenderdoc\Tests\texture_batch_fixture.cpp /Fe:x64\Development\texture_batch_fixture.exe /Fo:x64\Development\texture_batch_fixture.obj /link d3d11.lib d3dcompiler.lib
x64\Development\texture_batch_fixture.exe "%CD%\x64\Development\renderdoc.dll" "%CD%\x64\Development\texture_batch_fixture"
set RENDERDOC_TEXTURE_TEST_CAPTURE=%CD%\x64\Development\texture_batch_fixture_capture.rdc
x64\Development\qrenderdoc.exe --unittest log=texture-batch-tests.log "[texture-batch],[texture-batch-capture]"
```

Use the capture path printed by the fixture if RenderDoc chooses another name.
The fixture requires a D3D11-capable GPU. It binds the same two-mip texture twice,
updates mip 0 between draws, and includes BC1 sRGB, floating-point and depth
resources. Tests cover binding references, mip-0-only DDS contents, PNG direction,
compressed/sRGB/HDR data preservation in DDS, EXR output, snapshot deduplication,
first-event mode, overwrite protection, cancellation, partial failure and replay
restoration. These tests do not constitute cross-API or interactive UI validation.
