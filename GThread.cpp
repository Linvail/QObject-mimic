#include "GThread.h"
#include "GAbstractEventDispatcher.h"
#include <iostream>

thread_local GThread* GThread::s_currentThread = nullptr;

GThread::GThread(GObject* parent) : GObject(parent), m_dispatcher(nullptr) {
}

GThread::~GThread() {
    wait();
}

void GThread::start() {
    m_thread = std::make_unique<std::thread>([this]() {
        s_currentThread = this;
        GAbstractEventDispatcher dispatcher;
        m_dispatcher.store(&dispatcher);

        this->run();

        m_dispatcher.store(nullptr);
        s_currentThread = nullptr;
    });
}

void GThread::quit() {
    GAbstractEventDispatcher* dispatcher = m_dispatcher.load();
    if (dispatcher) {
        dispatcher->interrupt();
    }
}

void GThread::wait() {
    if (m_thread && m_thread->joinable()) {
        m_thread->join();
    }
}

GThread* GThread::currentThread() {
    return s_currentThread;
}

GAbstractEventDispatcher* GThread::eventDispatcher() const {
    return m_dispatcher.load();
}

void GThread::run() {
    exec();
}

int GThread::exec() {
    GAbstractEventDispatcher* dispatcher = m_dispatcher.load();
    if (dispatcher) {
        dispatcher->processEvents();
    }
    return 0;
}
