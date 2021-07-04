#include "FaustProcessor.h"

#ifdef BUILD_DAWDREAMER_FAUST

#include "faust/midi/RtMidi.cpp"

#ifndef WIN32
std::list<GUI*> GUI::fGuiList;
ztimedmap GUI::gTimedZoneMap;
#else
__declspec(selectany) std::list<GUI*> GUI::fGuiList;
__declspec(selectany) ztimedmap GUI::gTimedZoneMap;
#endif

#ifndef SAFE_DELETE
#define SAFE_DELETE(x)              do { if(x){ delete x; x = NULL; } } while(0)
#define SAFE_DELETE_ARRAY(x)        do { if(x){ delete [] x; x = NULL; } } while(0)
#define SAFE_RELEASE(x)             do { if(x){ x->release(); x = NULL; } } while(0)
#define SAFE_ADD_REF(x)             do { if(x){ x->add_ref(); } } while(0)
#define SAFE_REF_ASSIGN(lhs,rhs)    do { SAFE_RELEASE(lhs); (lhs) = (rhs); SAFE_ADD_REF(lhs); } while(0)
#endif

FaustProcessor::FaustProcessor(std::string newUniqueName, double sampleRate, int samplesPerBlock, std::string path) : ProcessorBase{ newUniqueName }
{
	mySampleRate = sampleRate;

	m_factory = NULL;
	m_poly_factory = NULL;
	m_dsp = NULL;
	m_dsp_poly = NULL;
	m_ui = NULL;
	m_midi_ui = NULL;
	m_numInputChannels = 0;
	m_numOutputChannels = 0;
	// auto import
	m_autoImport = "// FaustProcessor (DawDreamer) auto import:\nimport(\"stdfaust.lib\");\n";

	if (std::strcmp(path.c_str(), "") != 0) {
		this->compileFromFile(path);
	}
}

FaustProcessor::~FaustProcessor() {
	clear();
}

void
FaustProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
}

void
FaustProcessor::processBlock(juce::AudioSampleBuffer& buffer, juce::MidiBuffer&)
{
	automateParameters();

	AudioPlayHead::CurrentPositionInfo posInfo;
	getPlayHead()->getCurrentPosition(posInfo);

	if (m_nvoices < 1) {
		if (m_dsp != NULL) {
			m_dsp->compute(buffer.getNumSamples(), (float**)buffer.getArrayOfReadPointers(), buffer.getArrayOfWritePointers());
		}
	}
	else {
		long long int start = posInfo.timeInSamples;

		// render one sample at a time

		for (size_t i = 0; i < buffer.getNumSamples(); i++)
		{
			myIsMessageBetween = myMidiMessagePosition >= start && myMidiMessagePosition < start + 1;
			do {
				if (myIsMessageBetween) {

					int midiChannel = 0;
					if (myMidiMessage.isNoteOn()) {
						m_dsp_poly->keyOn(midiChannel, myMidiMessage.getNoteNumber(), myMidiMessage.getVelocity());
					}
					else {
						m_dsp_poly->keyOff(midiChannel, myMidiMessage.getNoteNumber(), myMidiMessage.getVelocity());
					}
					
					myMidiEventsDoRemain = myMidiIterator->getNextEvent(myMidiMessage, myMidiMessagePosition);
					myIsMessageBetween = myMidiMessagePosition >= start && myMidiMessagePosition < start + 1;
				}
			} while (myIsMessageBetween && myMidiEventsDoRemain);

			for (size_t chan = 0; chan < m_numInputChannels; chan++)
			{
				oneSampleInBuffer.setSample(chan, 0, buffer.getSample(chan, i));
			}

			m_dsp_poly->compute(1, (float**) oneSampleInBuffer.getArrayOfReadPointers(), (float**) oneSampleOutBuffer.getArrayOfWritePointers());

			for (size_t chan = 0; chan < m_numOutputChannels; chan++)
			{
				buffer.setSample(chan, i, oneSampleOutBuffer.getSample(chan, 0));
			}

			start += 1;
		}
	}
}

void
FaustProcessor::automateParameters() {

    AudioPlayHead::CurrentPositionInfo posInfo;
    getPlayHead()->getCurrentPosition(posInfo);

	if (m_dsp) {

		for (int i = 0; i < this->getNumParameters(); i++) {

			auto theName = this->getParameterName(i);

			auto theParameter = ((AutomateParameterFloat*)myParameters.getParameter(theName));
			if (theParameter) {
				m_ui->setParamValue(theName.toStdString().c_str(), theParameter->sample(posInfo.timeInSamples));
			}
			else {
				std::cout << "Error automateParameters: " << theName << std::endl;
			}
		}
	}

}

void
FaustProcessor::reset()
{
	if (m_dsp) {
		m_dsp->instanceClear();
	}

	if (myMidiIterator) {
		delete myMidiIterator;
	}

	myMidiIterator = new MidiBuffer::Iterator(myMidiBuffer); // todo: deprecated.
	myMidiEventsDoRemain = myMidiIterator->getNextEvent(myMidiMessage, myMidiMessagePosition);
}

void
FaustProcessor::clear()
{
	m_numInputChannels = 0;
	m_numOutputChannels = 0;

	// todo: do something with m_midi_handler
	if (m_dsp_poly) {
		m_midi_handler.removeMidiIn(m_dsp_poly);
		m_midi_handler.stopMidi();
	}
	if (m_midi_ui) {
		m_midi_ui->removeMidiIn(m_dsp_poly);
		m_midi_ui->stop();
	}

	SAFE_DELETE(m_dsp);
	SAFE_DELETE(m_ui);
	SAFE_DELETE(m_dsp_poly);
	SAFE_DELETE(m_midi_ui);

	deleteAllDSPFactories();
	m_factory = NULL;
	m_poly_factory = NULL;
}

bool
FaustProcessor::setNumVoices(int numVoices) {
	m_nvoices = std::max(0, numVoices);
	// recompile
	return compileFromString(m_code);
}

int
FaustProcessor::getNumVoices(int numVoices) {
	return m_nvoices;
}

bool
FaustProcessor::compileFromString(const std::string& code)
{
    m_isCompiled = false;
    
	// clean up
	clear();

	if (std::strcmp(code.c_str(), "") == 0) {
		return false;
	}

	// arguments
	const int argc = 0;
	const char** argv = NULL;
	// optimization level
	const int optimize = -1;

	// save
	m_code = code;
    auto theCode = m_autoImport + "\n" + code;

	std::string m_errorString;

	// create new factory
	bool is_polyphonic = m_nvoices > 0;
	if (is_polyphonic) {
		m_poly_factory = createPolyDSPFactoryFromString("DawDreamer", theCode,
			argc, argv, "", m_errorString, optimize);
	}
	else {
		m_factory = createDSPFactoryFromString("DawDreamer", theCode,
			argc, argv, "", m_errorString, optimize);
	}

	// check for error
	if (m_errorString != "") {
		// output error
		std::cerr << "[Faust]: " << m_errorString << std::endl;
		// clear
		clear();
		// done
		return false;
	}

	//// print where faustlib is looking for stdfaust.lib and the other lib files.
	//auto pathnames = m_factory->getIncludePathnames();
	//std::cout << "pathnames:\n" << std::endl;
	//for (auto name : pathnames) {
	//	std::cout << name << "\n" << std::endl;
	//}
	//std::cout << "library list:\n" << std::endl;
	//auto librarylist = m_factory->getLibraryList();
	//for (auto name : librarylist) {
	//	std::cout << name << "\n" << std::endl;
	//}

	if (is_polyphonic) {
		// (false, true) works
		m_dsp_poly = m_poly_factory->createPolyDSPInstance(m_nvoices, false, false);
		if (!m_dsp_poly) {
			std::cerr << "Cannot create instance " << std::endl;
			clear();
			return false;
		}
	}
	else {
		// create DSP instance
		m_dsp = m_factory->createDSPInstance();
		if (!m_dsp) {
			std::cerr << "Cannot create instance " << std::endl;
			clear();
			return false;
		}
	}

	dsp* theDsp = is_polyphonic ? m_dsp_poly : m_dsp;

	// get channels
	int inputs = theDsp->getNumInputs();
	int outputs = theDsp->getNumOutputs();

	if (outputs != 2) {
		std::cerr << "FaustProcessor must have DSP code with 2 output channels but was compiled for " << m_numOutputChannels << "." << std::endl;
		clear();
		return false;
	}

	m_numInputChannels = inputs;
	m_numOutputChannels = outputs;

	// make new UI
	UI* theUI = nullptr;
	if (is_polyphonic)
	{
		m_midi_handler = rt_midi("my_midi");
		m_midi_handler.addMidiIn(m_dsp_poly);

		m_midi_ui = new MidiUI(&m_midi_handler);
		theUI = m_midi_ui;

		oneSampleInBuffer.setSize(m_numInputChannels, 1);
		oneSampleOutBuffer.setSize(m_numOutputChannels, 1);
	}
	else {
		m_ui = new APIUI();
		theUI = m_ui;
	}

	// build ui
	theDsp->buildUserInterface(theUI);

	// init
	theDsp->init((int)(mySampleRate + .5));

	if (is_polyphonic) {
		m_midi_ui->run();
	}

	createParameterLayout();

    m_isCompiled = true;
	return true;
}

bool
FaustProcessor::compileFromFile(const std::string& path)
{
	if (std::strcmp(path.c_str(), "") == 0) {
        return false;
    }
    
	// open file
	std::ifstream fin(path.c_str());
	// check
	if (!fin.good())
	{
		// error
		std::cerr << "[Faust]: ERROR opening file: '" << path << "'" << std::endl;
		return false;
	}

	// clear code string
	m_code = "";
	// get it
	for (std::string line; std::getline(fin, line); ) {
		m_code += line + '\n';
	}
	// eval it
	return compileFromString(m_code);
}

float
FaustProcessor::setParamWithPath(const std::string& n, float p)
{
	// sanity check
	if (!m_ui) return 0;

	// set the value
	m_ui->setParamValue(n.c_str(), p);

	return p;
}

float
FaustProcessor::getParamWithPath(const std::string& n)
{
	if (!m_ui) return 0; // todo: better handling
	return m_ui->getParamValue(n.c_str());
}

float
FaustProcessor::setParamWithIndex(const int index, float p)
{
	// sanity check
	if (!m_ui) return 0;

	// set the value
	m_ui->setParamValue(index, p);

	return p;
}

float
FaustProcessor::getParamWithIndex(const int index)
{
	if (!m_ui) return 0; // todo: better handling
	return m_ui->getParamValue(index);
}

std::string
FaustProcessor::code()
{
	return m_code;
}

void
FaustProcessor::createParameterLayout()
{
	juce::AudioProcessorValueTreeState::ParameterLayout blankLayout;

	// clear existing parameters in the layout?
	ValueTree blankState;
	myParameters.replaceState(blankState);

	if (m_nvoices != 0) {
		return;
	}

	for (int i = 0; i < m_ui->getParamsCount(); ++i)
	{
		auto parameterName = m_ui->getParamAddress(i);
		auto parameterLabel = m_ui->getParamLabel(i);
		myParameters.createAndAddParameter(std::make_unique<AutomateParameterFloat>(parameterName, parameterName,
			NormalisableRange<float>(m_ui->getParamMin(i), m_ui->getParamMax(i)), m_ui->getParamInit(i), parameterLabel));
		// give it a valid single sample of automation.
		ProcessorBase::setAutomationVal(parameterName, m_ui->getParamValue(i));
	}
}

py::list
FaustProcessor::getPluginParametersDescription()
{
	py::list myList;

	if (m_dsp != nullptr) {

		for (int i = 0; i < m_ui->getParamsCount(); i++) {

			py::dict myDictionary;
			myDictionary["index"] = i;
			myDictionary["name"] = m_ui->getParamAddress(i);;
			myDictionary["min"] = m_ui->getParamMin(i);
			myDictionary["max"] = m_ui->getParamMax(i);
			myDictionary["label"] = m_ui->getParamLabel(i);
			myDictionary["step"] = m_ui->getParamStep(i);
			myDictionary["value"] = m_ui->getParamValue(i);

			myList.append(myDictionary);
		}
	}
	else
	{
		std::cout << "[FaustProcessor] Please compile DSP code first!" << std::endl;
	}

	return myList;
}

int
FaustProcessor::getNumMidiEvents()
{
	return myMidiBuffer.getNumEvents();
};

bool
FaustProcessor::loadMidi(const std::string& path)
{
	File file = File(path);
	FileInputStream fileStream(file);
	MidiFile midiFile;
	midiFile.readFrom(fileStream);
	midiFile.convertTimestampTicksToSeconds();
	myMidiBuffer.clear();

	for (int t = 0; t < midiFile.getNumTracks(); t++) {
		const MidiMessageSequence* track = midiFile.getTrack(t);
		for (int i = 0; i < track->getNumEvents(); i++) {
			MidiMessage& m = track->getEventPointer(i)->message;
			int sampleOffset = (int)(mySampleRate * m.getTimeStamp());
			myMidiBuffer.addEvent(m, sampleOffset);
		}
	}

	return true;
}

void
FaustProcessor::clearMidi() {
	myMidiBuffer.clear();
}

bool
FaustProcessor::addMidiNote(uint8  midiNote,
	uint8  midiVelocity,
	const double noteStart,
	const double noteLength) {

	if (midiNote > 255) midiNote = 255;
	if (midiNote < 0) midiNote = 0;
	if (midiVelocity > 255) midiVelocity = 255;
	if (midiVelocity < 0) midiVelocity = 0;
	if (noteLength <= 0) {
		return false;
	}

	// Get the note on midiBuffer.
	MidiMessage onMessage = MidiMessage::noteOn(1,
		midiNote,
		midiVelocity);

	MidiMessage offMessage = MidiMessage::noteOff(1,
		midiNote,
		midiVelocity);

	auto startTime = noteStart * mySampleRate;
	onMessage.setTimeStamp(startTime);
	offMessage.setTimeStamp(startTime + noteLength * mySampleRate);
	myMidiBuffer.addEvent(onMessage, (int)onMessage.getTimeStamp());
	myMidiBuffer.addEvent(offMessage, (int)offMessage.getTimeStamp());

	return true;
}

#endif
