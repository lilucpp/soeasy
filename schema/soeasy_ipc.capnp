@0xb8a9c4d2e1f03756;

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("soeasy::ipc");

enum ParseLevel {
  l1Text    @0;
  l2Office  @1;
  l3Archive @2;
  l4Ocr     @3;
}

enum ParseStatusCode {
  ok          @0;
  failed      @1;
  timeout     @2;
  unsupported @3;
  crashed     @4;
  encrypted   @5;
  partialOcr  @6;
  emptyContent @7;
}

enum ArchiveItemEvent {
  data            @0;
  encrypted       @1;
  skippedTooLarge @2;
  failed          @3;
  heartbeat       @4;
}

interface TextStreamReceiver {
  sendChunk @0 (chunk :Data) -> ();
  finish    @1 (totalBytes :UInt64, isScannedPdf :Bool, statusCode :ParseStatusCode, errorMsg :Text) -> ();
}

interface ArchiveStreamReceiver {
  onItemMeta  @0 (virtualPath :Text,
                  fileName    :Text,
                  ext         :Text,
                  entrySize   :UInt64,
                  entryMtime  :Int64,
                  event       :ArchiveItemEvent,
                  message     :Text) -> (keepGoing :Bool);
  onItemChunk @1 (chunk :Data) -> ();
  onItemEnd   @2 () -> (keepGoing :Bool);
  finish      @3 (statusCode :ParseStatusCode, message :Text) -> ();
}

interface BufferParseSession {
  sendChunk @0 (chunk :Data) -> ();
  finish    @1 (receiver :TextStreamReceiver) -> ();
}

interface WorkerAgent {
  parseFileStream      @0 (docId :Int64, filePath :Text, ext :Text, level :ParseLevel, receiver :TextStreamReceiver, timeoutMs :UInt32)
                       -> ();
  ping                 @1 () -> (rssBytes :UInt64, isBusy :Bool);
  beginBufferParse     @2 (docId :Int64, filePath :Text, ext :Text, totalBytes :UInt64, level :ParseLevel, timeoutMs :UInt32)
                       -> (session :BufferParseSession);
  extractArchiveStream @3 (archivePath :Text, enabledExts :List(Text), maxEntryBytes :UInt64, receiver :ArchiveStreamReceiver)
                       -> ();
}
