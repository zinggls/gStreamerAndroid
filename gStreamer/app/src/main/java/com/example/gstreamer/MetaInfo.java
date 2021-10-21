package com.example.gstreamer;

public class MetaInfo {
    String name;
    long size;
    int fd;

    MetaInfo(String name,Long size,int fd){ this.name=name; this.size=size; this.fd = fd; }
}
