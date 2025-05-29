// BaseApp.h
#pragma once

#include <Arduino.h>

class BaseApp {
public:
    virtual void init() = 0;
    virtual void loop() = 0;
    virtual void redraw(bool force = false, int xOffset = 0) = 0;
    virtual void setNeedsRedraw(bool flag) = 0;
    virtual bool getNeedsRedraw() = 0;
    virtual ~BaseApp() {}
    virtual String getAppId() = 0;
};