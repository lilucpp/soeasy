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
}

struct ShmSlotRef {
  shmName   @0 :Text;
  slotId    @1 :UInt32;
  dataSize  @2 :UInt64;
}

struct ParseRequest {
  docId       @0 :Int64;
  filePath    @1 :Text;
  ext         @2 :Text;
  level       @3 :ParseLevel;
  targetSlot  @4 :ShmSlotRef;
}

struct ParseResult {
  docId           @0 :Int64;
  statusCode      @1 :ParseStatusCode;
  extractedBytes  @2 :UInt64;
  pageCount       @3 :UInt32;
  isScannedPdf    @4 :Bool;
  message         @5 :Text;
  chunks          @6 :List(Data);
  extractedText   @7 :Text;
}

struct WorkerPingResponse {
  rssBytes  @0 :UInt64;
  isBusy    @1 :Bool;
}

interface TextStreamReceiver {
  sendChunk @0 (chunk :Data) -> ();
  finish    @1 (totalBytes :UInt64, isScannedPdf :Bool, statusCode :Int32, errorMsg :Text) -> ();
}

interface WorkerAgent {
  parseFile       @0 (docId :Int64, filePath :Text, ext :Text, level :ParseLevel, targetSlot :ShmSlotRef)
                  -> (result :ParseResult);
  parseFileStream @1 (docId :Int64, filePath :Text, ext :Text, level :ParseLevel, receiver :TextStreamReceiver)
                  -> ();
  ping            @2 () -> (rssBytes :UInt64, isBusy :Bool);
}
