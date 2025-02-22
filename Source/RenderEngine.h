#pragma once

#include <random>
#include <array>
#include <iomanip>
#include <sstream>
#include <string>

#include "AllProcessors.h"

class DAGNode {
public:
    ProcessorBase* processorBase;
    std::vector<std::string> inputs;
};

class DAG {
public:
    std::vector<DAGNode> nodes;
};

class RenderEngine : AudioPlayHead
{
public:
    RenderEngine(double sr, int bs);
    ~RenderEngine();
    // RenderEngine(const RenderEngine&) = delete;

    bool loadGraph(DAG dagNodes, int numInputAudioChans, int numOutputAudioChans);
    
    void render (const double renderLength);

    void setBPM(double bpm);

    py::array_t<float> getAudioFrames();

    py::array_t<float> getAudioFramesForName(std::string& name);

    bool getCurrentPosition(CurrentPositionInfo& result) override;
    bool canControlTransport() override;
    void transportPlay(bool shouldStartPlaying) override;
    void transportRecord(bool shouldStartRecording) override;
    void transportRewind() override;

protected:

    double mySampleRate;
    int myBufferSize;
    double myBPM = 120.;

private:
                           
    std::vector<std::vector<float>> myRecordedSamples;

    std::unique_ptr<juce::AudioProcessorGraph> myMainProcessorGraph;

    juce::AudioProcessorGraph::Node::Ptr myMidiInputNode;

    int myNumInputAudioChans = 2;
    int myNumOutputAudioChans = 2;

    CurrentPositionInfo myCurrentPositionInfo;

};
