// Kenton's Code Playground -- http://code.google.com/p/kentons-code
// Author: Kenton Varda (temporal@gmail.com)
// Copyright (c) 2010 Google, Inc. and contributors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "ProtoDashboard.h"

#include <errno.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <google/protobuf/io/coded_stream.h>

#include "dashboard.pb.h"
#include "os/Socket.h"
#include "MuxDashboard.h"

namespace ekam {

class ProtoDashboard::TaskImpl : public Dashboard::Task {
public:
  TaskImpl(int id, const std::string& verb, const std::string& noun,
           Silence silence, WriteBuffer* output);
  ~TaskImpl();

  // implements Task ---------------------------------------------------------------------
  void setState(TaskState state);
  void addOutput(const std::string& text);

private:
  int id;
  WriteBuffer* output;

  static const proto::TaskUpdate::State STATE_CODES[];
};

const proto::TaskUpdate::State ProtoDashboard::TaskImpl::STATE_CODES[] = {
  proto::TaskUpdate::PENDING,
  proto::TaskUpdate::RUNNING,
  proto::TaskUpdate::DONE   ,
  proto::TaskUpdate::PASSED ,
  proto::TaskUpdate::FAILED ,
  proto::TaskUpdate::BLOCKED
};

ProtoDashboard::TaskImpl::TaskImpl(int id, const std::string& verb, const std::string& noun,
                                   Silence silence, WriteBuffer* output)
    : id(id), output(output) {
  proto::TaskUpdate update;
  update.set_id(id);
  update.set_state(proto::TaskUpdate::PENDING);
  update.set_verb(verb);
  update.set_noun(noun);
  update.set_silent(silence == SILENT);
  output->write(update);
}

ProtoDashboard::TaskImpl::~TaskImpl() {
  proto::TaskUpdate update;
  update.set_id(id);
  update.set_state(proto::TaskUpdate::DELETED);
  output->write(update);
}

void ProtoDashboard::TaskImpl::setState(TaskState state) {
  proto::TaskUpdate update;
  update.set_id(id);
  update.set_state(STATE_CODES[state]);
  output->write(update);
}

void ProtoDashboard::TaskImpl::addOutput(const std::string& text) {
  proto::TaskUpdate update;
  update.set_id(id);
  update.set_log(text);
  output->write(update);
}

// =======================================================================================

ProtoDashboard::ProtoDashboard(EventManager* eventManager, OwnedPtr<ByteStream> stream)
    : idCounter(0),
      writeBuffer(eventManager, stream.release()) {
  proto::Header header;
  char* cwd = get_current_dir_name();
  header.set_project_root(cwd);
  free(cwd);
  writeBuffer.write(header);
}
ProtoDashboard::~ProtoDashboard() {}

OwnedPtr<Dashboard::Task> ProtoDashboard::beginTask(
    const std::string& verb, const std::string& noun, Silence silence) {
  return newOwned<TaskImpl>(++idCounter, verb, noun, silence, &writeBuffer);
}

// =======================================================================================

ProtoDashboard::WriteBuffer::WriteBuffer(EventManager* eventManager,
                                         OwnedPtr<ByteStream> stream)
    : eventManager(eventManager), stream(stream.release()),
      ioWatcher(eventManager->watchFd(this->stream->getHandle()->get())),
      offset(0), disconnectFulfiller(NULL) {}
ProtoDashboard::WriteBuffer::~WriteBuffer() {}

void ProtoDashboard::WriteBuffer::write(const google::protobuf::MessageLite& message) {
  if (stream == NULL) {
    // Already disconnected.
    return;
  }

  using google::protobuf::io::CodedOutputStream;
  using google::protobuf::uint8;

  messages.push(std::string());
  std::string* data = &messages.back();

  // TODO:  This should really be a helper function in the protobuf library...
  {
    int size = message.ByteSize();
    data->resize(size + CodedOutputStream::VarintSize32(size));
    uint8* ptr = reinterpret_cast<uint8*>(&*data->begin());

    ptr = CodedOutputStream::WriteVarint32ToArray(size, ptr);
    ptr = message.SerializeWithCachedSizesToArray(ptr);
    assert(ptr == reinterpret_cast<const uint8*>(data->data() + data->size()));
  }

  ready();
}

void ProtoDashboard::WriteBuffer::ready() {
  try {
    while (!messages.empty()) {
      const std::string& message = messages.front();
      while (offset < message.size()) {
        offset += stream->write(message.data() + offset, message.size() - offset);
      }
      offset = 0;
      messages.pop();
    }
  } catch (const OsError& error) {
    if (error.getErrorNumber() == EAGAIN) {
      // Ran out of kernel buffer space.  Wait until writable again.
      waitWritablePromise = eventManager->when(ioWatcher->onWritable())(
        [this](Void) {
          ready();
        });
    } else {
      stream.clear();

      if (disconnectFulfiller != NULL) {
        disconnectFulfiller->disconnected();
      }
    }
  }
}

// =======================================================================================

ProtoDashboard::WriteBuffer::DisconnectFulfiller::DisconnectFulfiller(Callback* callback,
                                                                      WriteBuffer* writeBuffer)
    : callback(callback), writeBuffer(writeBuffer) {
  if (writeBuffer->disconnectFulfiller != NULL) {
    throw std::logic_error("Can only register one disconnect callback at a time.");
  }
  writeBuffer->disconnectFulfiller = this;
}

ProtoDashboard::WriteBuffer::DisconnectFulfiller::~DisconnectFulfiller() {
  assert(writeBuffer->disconnectFulfiller == this);
  writeBuffer->disconnectFulfiller = NULL;
}

Promise<void> ProtoDashboard::onDisconnect() {
  return writeBuffer.onDisconnect();
}

Promise<void> ProtoDashboard::WriteBuffer::onDisconnect() {
  return newPromise<DisconnectFulfiller>(this);
}

// =======================================================================================

class NetworkAcceptingDashboard : public Dashboard {
public:
  NetworkAcceptingDashboard(EventManager* eventManager, const std::string& address,
                            OwnedPtr<Dashboard> baseDashboard)
      : eventManager(eventManager),
        base(baseDashboard.release()),
        baseConnector(newOwned<MuxDashboard::Connector>(&mux, base.get())),
        socket(newOwned<ServerSocket>(eventManager, address)),
        acceptOp(doAccept()) {}
  ~NetworkAcceptingDashboard() {}

  Promise<void> doAccept() {
    return eventManager->when(socket->accept())(
      [this](OwnedPtr<ByteStream> stream){
        accepted(stream.release());
        return doAccept();
      });
  }

  void accepted(OwnedPtr<ByteStream> stream);

  // implements Dashboard ----------------------------------------------------------------
  OwnedPtr<Task> beginTask(const std::string& verb, const std::string& noun, Silence silence) {
    return mux.beginTask(verb, noun, silence);
  }

private:
  EventManager* eventManager;
  OwnedPtr<Dashboard> base;
  MuxDashboard mux;
  OwnedPtr<MuxDashboard::Connector> baseConnector;
  OwnedPtr<ServerSocket> socket;
  Promise<void> acceptOp;

  class ConnectedProtoDashboard {
  public:
    ConnectedProtoDashboard(NetworkAcceptingDashboard* owner, EventManager* eventManager,
                            OwnedPtr<ByteStream> stream)
        : protoDashboard(eventManager, stream.release()),
          connector(newOwned<MuxDashboard::Connector>(&owner->mux, &protoDashboard)) {
      disconnectPromise = eventManager->when(protoDashboard.onDisconnect())(
        [this, owner](Void) {
          connector.clear();
          owner->connectedDashboards.erase(this);
        });
    }
    ~ConnectedProtoDashboard() {}

  private:
    ProtoDashboard protoDashboard;
    OwnedPtr<MuxDashboard::Connector> connector;
    Promise<void> disconnectPromise;
  };
  OwnedPtrMap<ConnectedProtoDashboard*, ConnectedProtoDashboard> connectedDashboards;
};

void NetworkAcceptingDashboard::accepted(OwnedPtr<ByteStream> stream) {
  auto connectedDashboard = newOwned<ConnectedProtoDashboard>(this, eventManager, stream.release());
  auto key = connectedDashboard.get();  // cannot inline due to undefined evaluation order
  connectedDashboards.add(key, connectedDashboard.release());
}

OwnedPtr<Dashboard> initNetworkDashboard(EventManager* eventManager, const std::string& address,
                                         OwnedPtr<Dashboard> dashboardToWrap) {
  return newOwned<NetworkAcceptingDashboard>(eventManager, address, dashboardToWrap.release());
}

}  // namespace ekam
