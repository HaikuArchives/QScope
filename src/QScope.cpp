/*
 *  QScope - Triggered stereo oscilloscope
 *
 *  Written in 1997 by Christian Bauer
 */

#include <AppKit.h>
#include <InterfaceKit.h>
#include <MediaKit.h>

#include "TSliderView.h"


// Constants
const char APP_SIGNATURE[] = "application/x-vnd.cebix-QScope";

const uint32 MSG_NEW_BUFFER = 'nbuf';
const uint32 MSG_DAC_STREAM = 'dacs';
const uint32 MSG_ADC_STREAM = 'adcs';
const uint32 MSG_LEFT_CHANNEL = 'left';
const uint32 MSG_RIGHT_CHANNEL = 'rght';
const uint32 MSG_STEREO_CHANNELS = 'dual';
const uint32 MSG_TIME_DIV_100us = '100u';
const uint32 MSG_TIME_DIV_200us = '200u';
const uint32 MSG_TIME_DIV_500us = '500u';
const uint32 MSG_TIME_DIV_1ms = '1ms ';
const uint32 MSG_TIME_DIV_2ms = '2ms ';
const uint32 MSG_TIME_DIV_5ms = '5ms ';
const uint32 MSG_TIME_DIV_10ms = '10ms';
const uint32 MSG_TRIGGER_OFF = 'trof';
const uint32 MSG_TRIGGER_LEVEL = 'trlv';
const uint32 MSG_TRIGGER_PEAK= 'trpk';
const uint32 MSG_TRIGGER_LEFT = 'trlt';
const uint32 MSG_TRIGGER_RIGHT = 'trrt';
const uint32 MSG_SLOPE_POS = 'slp+';
const uint32 MSG_SLOPE_NEG = 'slp-';
const uint32 MSG_ILLUMINATION = 'illu';

const int SCOPE_WIDTH = 320;	// Scope grid parameters
const int SCOPE_HEIGHT = 256;
const int NUM_X_DIVS = 10;
const int NUM_Y_DIVS = 8;
const int TICKS_PER_DIV = 5;

const float SAMPLE_RATE = 44100.0;

enum {	// Subscriber states
	STATE_HOLD_OFF,
	STATE_WAIT_FOR_TRIGGER,
	STATE_RECORD
};

enum {	// Trigger modes
	TRIGGER_OFF,
	TRIGGER_LEVEL,
	TRIGGER_PEAK
};

const rgb_color fill_color = {216, 216, 216, 0};


// Global variables
uint8 c_black, c_dark_green;	// Scope colors
uint8 c_beam[16];


// QScope audio stream subscriber
class QScopeSubscriber : public BSubscriber {
public:
	QScopeSubscriber(BLooper *looper);
	~QScopeSubscriber();
	void Enter(BAbstractBufferStream *stream);
	void SetTimePerDiv(float time);
	void SetTriggerMode(int mode);
	void SetHoldOff(float time);

	bool TriggerRightChannel;
	bool TriggerSlopeNeg;
	int TriggerLevel;

private:
	static bool stream_func(void *arg, char *buf, size_t count, void *header);
	void scope_func(int16 *buf, size_t count);

	BLooper *the_looper;
	BAbstractBufferStream *the_stream;

	int16 scope_buf[2][SCOPE_WIDTH*4];	// Two buffers containing min/max values for left/right channels
	int active_buf;			// For double buffering

	int state;				// Current state (STATE_...)

	int scope_counter;				// Number of samples accumulated in scope_buf
	int record_counter;				// Current sample frame index in input buffer
	float time_per_div;				// Time per division
	float next_frame;				// Next sample frame in input buffer
	float frame_add;				// Added to next_frame for each scope_buf sample
	int16 old_input;				// Previous input for trigger slope detection
	int16 left_min, left_max;		// Current minimum/maximum sample elongation
	int16 right_min, right_max;		// Current minimum/maximum sample elongation
	int16 left_peak, right_peak;	// Peak levels found during recording

	float hold_off;				// Hold-off time in multiples of the time/div time
	int hold_off_frames;		// Number of sample frames to hold off
	int hold_off_counter;		// Counter for remaining number of sample frames to wait

	int trigger_start_frame;	// First sample frame index for trigger
	int trigger_total_frames;	// Total number of frames waited for trigger
	int trigger_mode;			// Trigger mode (TRIGGER_...)
};


// Bitmap view
class BitmapView : public BView {
	BBitmap *the_bitmap;
public:
	BitmapView(BRect frame, BBitmap *bitmap) : BView(frame, "bitmap", B_FOLLOW_ALL_SIDES, B_WILL_DRAW), the_bitmap(bitmap) {}
	virtual void Draw(BRect update) {DrawBitmap(the_bitmap, update, update);}
};


// Looper for drawing the scope
class DrawLooper : public BLooper {
public:
	DrawLooper(BitmapView *view, BBitmap *bitmap);
	virtual void MessageReceived(BMessage *msg);

	bool Stereo;		// Stereo display
	bool RightChannel;	// Right/left channel

private:
	void draw_data(int16 *buf, int y_offset, int y_height);

	BitmapView *the_view;
	BRect the_bounds;
	BWindow *the_window;
	BBitmap *the_bitmap;
	uint8 *bits;
	int xmod;
};


// Window object
class QScopeWindow : public BWindow {
public:
	QScopeWindow();
	virtual bool QuitRequested(void);
	virtual void MessageReceived(BMessage *msg);

private:
	static void trigger_level_callback(float value, void *arg);
	static void hold_off_callback(float value, void *arg);

	BitmapView *main_view;
	BBitmap *the_bitmap;

	DrawLooper *the_looper;

	BDACStream *dac_stream;
	BADCStream *adc_stream;
	QScopeSubscriber *the_subscriber;

	friend class TSliderView;

	bool illumination;		// Backlight
};


// Application object
class QScope : public BApplication {
public:
	QScope() : BApplication(APP_SIGNATURE) {}
	virtual void ReadyToRun(void) {new QScopeWindow;}
	virtual void AboutRequested(void);
};


/*
 *  Create application object and start it
 */

int main(int argc, char **argv)
{	
	QScope *the_app = new QScope();
	the_app->Run();
	delete the_app;
	return 0;
}


/*
 *  About requested
 */

void QScope::AboutRequested(void)
{
	BAlert *the_alert = new BAlert("",
		"QScope by Christian Bauer\n"
		"<cbauer@iphcip1.physik.uni-mainz.de>\n"
		"Public domain.",
		"Neat");
	the_alert->Go();
}


/*
 *  Window constructor
 */

QScopeWindow::QScopeWindow() : BWindow(BRect(0, 0, SCOPE_WIDTH+200-1, SCOPE_HEIGHT-1), "QScope", B_TITLED_WINDOW, B_NOT_RESIZABLE)
{
	// Move window to right position
	Lock();
	MoveTo(80, 60);
	BRect b = Bounds();

	// Look up colors for scope
	{
		BScreen scr(this);
		illumination = false;
		c_black = scr.IndexForColor(0, 0, 0);
		c_dark_green = scr.IndexForColor(0, 32, 16);
		for (int i=0; i<16; i++)
			c_beam[i] = scr.IndexForColor(0, 255 - i * 8, 128 - i * 4);
	}

	// Light gray background
	BView *top = new BView(BRect(0, 0, b.right, b.bottom), "top", B_FOLLOW_NONE, B_WILL_DRAW);
	AddChild(top);
	top->SetViewColor(fill_color);

	// Allocate bitmap
	the_bitmap = new BBitmap(BRect(0, 0, SCOPE_WIDTH-1, SCOPE_HEIGHT-1), B_COLOR_8_BIT);

	// Create bitmap view
	main_view = new BitmapView(BRect(0, 0, SCOPE_WIDTH-1, SCOPE_HEIGHT-1), the_bitmap);
	top->AddChild(main_view);

	// Create interface elements
	{
		BBox *box = new BBox(BRect(SCOPE_WIDTH + 4, 4, SCOPE_WIDTH + 196, 62));
		top->AddChild(box);
		box->SetLabel("Input");

		BPopUpMenu *popup = new BPopUpMenu("stream popup", true, true);
		popup->AddItem(new BMenuItem("DAC", new BMessage(MSG_DAC_STREAM)));
		popup->AddItem(new BMenuItem("ADC", new BMessage(MSG_ADC_STREAM)));
		popup->SetTargetForItems(this);
		popup->ItemAt(0)->SetMarked(true);
		BMenuField *menu_field = new BMenuField(BRect(4, 14, 188, 34), "stream", "Stream", popup);
		box->AddChild(menu_field);

		popup = new BPopUpMenu("channel popup", true, true);
		popup->AddItem(new BMenuItem("Left", new BMessage(MSG_LEFT_CHANNEL)));
		popup->AddItem(new BMenuItem("Right", new BMessage(MSG_RIGHT_CHANNEL)));
		popup->AddItem(new BMenuItem("Stereo", new BMessage(MSG_STEREO_CHANNELS)));
		popup->SetTargetForItems(this);
		popup->ItemAt(0)->SetMarked(true);
		menu_field = new BMenuField(BRect(4, 34, 188, 54), "channel", "Channel", popup);
		box->AddChild(menu_field);
	}

	{
		BBox *box = new BBox(BRect(SCOPE_WIDTH + 4, 66, SCOPE_WIDTH + 196, 104));
		top->AddChild(box);
		box->SetLabel("Time");

		BPopUpMenu *popup = new BPopUpMenu("time/div popup", true, true);
		popup->AddItem(new BMenuItem("0.1ms", new BMessage(MSG_TIME_DIV_100us)));
		popup->AddItem(new BMenuItem("0.2ms", new BMessage(MSG_TIME_DIV_200us)));
		popup->AddItem(new BMenuItem("0.5ms", new BMessage(MSG_TIME_DIV_500us)));
		popup->AddItem(new BMenuItem("1ms", new BMessage(MSG_TIME_DIV_1ms)));
		popup->AddItem(new BMenuItem("2ms", new BMessage(MSG_TIME_DIV_2ms)));
		popup->AddItem(new BMenuItem("5ms", new BMessage(MSG_TIME_DIV_5ms)));
		popup->AddItem(new BMenuItem("10ms", new BMessage(MSG_TIME_DIV_10ms)));
		popup->SetTargetForItems(this);
		popup->ItemAt(4)->SetMarked(true);
		BMenuField *menu_field = new BMenuField(BRect(4, 14, 188, 34), "time/div", "Time/Div.", popup);
		box->AddChild(menu_field);
	}

	{
		BBox *box = new BBox(BRect(SCOPE_WIDTH + 4, 108, SCOPE_WIDTH + 196, 230));
		top->AddChild(box);
		box->SetLabel("Trigger");

		BPopUpMenu *popup = new BPopUpMenu("trigger channel popup", true, true);
		popup->AddItem(new BMenuItem("Left", new BMessage(MSG_TRIGGER_LEFT)));
		popup->AddItem(new BMenuItem("Right", new BMessage(MSG_TRIGGER_RIGHT)));
		popup->SetTargetForItems(this);
		popup->ItemAt(0)->SetMarked(true);
		BMenuField *menu_field = new BMenuField(BRect(4, 14, 188, 34), "trigger_channel", "Channel", popup);
		box->AddChild(menu_field);

		popup = new BPopUpMenu("trigger mode popup", true, true);
		popup->AddItem(new BMenuItem("Off", new BMessage(MSG_TRIGGER_OFF)));
		popup->AddItem(new BMenuItem("Level", new BMessage(MSG_TRIGGER_LEVEL)));
		popup->AddItem(new BMenuItem("Peak", new BMessage(MSG_TRIGGER_PEAK)));
		popup->SetTargetForItems(this);
		popup->ItemAt(1)->SetMarked(true);
		menu_field = new BMenuField(BRect(4, 34, 188, 54), "trigger_mode", "Trigger Mode", popup);
		box->AddChild(menu_field);

		BStringView *label = new BStringView(BRect(5, 54, 97, 73), "", "Level");
		box->AddChild(label);
		TSliderView *the_slider = new TSliderView(BRect(98, 58, 188, 76), "level", 0.5, trigger_level_callback, this);
		box->AddChild(the_slider);

		popup = new BPopUpMenu("slope popup", true, true);
		popup->AddItem(new BMenuItem("pos", new BMessage(MSG_SLOPE_POS)));
		popup->AddItem(new BMenuItem("neg", new BMessage(MSG_SLOPE_NEG)));
		popup->SetTargetForItems(this);
		popup->ItemAt(0)->SetMarked(true);
		menu_field = new BMenuField(BRect(4, 76, 188, 96), "slope", "Slope", popup);
		box->AddChild(menu_field);

		label = new BStringView(BRect(5, 96, 97, 115), "", "Hold off");
		box->AddChild(label);
		the_slider = new TSliderView(BRect(98, 100, 188, 118), "hold_off", 0.0, hold_off_callback, this);
		box->AddChild(the_slider);
	}

	BCheckBox *check_box = new BCheckBox(BRect(SCOPE_WIDTH + 10, 234, SCOPE_WIDTH + 190, 254), "illumination", "Illumination", new BMessage(MSG_ILLUMINATION));
	top->AddChild(check_box);
	Unlock();

	// Create drawing looper
	the_looper = new DrawLooper(main_view, the_bitmap);

	// Create stream objects
	dac_stream = new BDACStream();
	adc_stream = new BADCStream();

	// Create subscriber and attach it to the stream
	the_subscriber = new QScopeSubscriber(the_looper);
	the_subscriber->Enter(dac_stream);

	// Show the window
	Show();
}


/*
 *  Quit requested
 */

bool QScopeWindow::QuitRequested(void)
{
	// Delete subscriber
	delete the_subscriber;

	// Delete stream objects
	delete dac_stream;
	delete adc_stream;

	// Delete looper
	the_looper->Lock();
	the_looper->Quit();

	// Delete the bitmap
	delete the_bitmap;

	// Quit program
	be_app->PostMessage(B_QUIT_REQUESTED);
	return TRUE;
}


/*
 *  Handle messages
 */

void QScopeWindow::MessageReceived(BMessage *msg)
{
	switch (msg->what) {
		case MSG_DAC_STREAM: the_subscriber->Enter(dac_stream); break;
		case MSG_ADC_STREAM: the_subscriber->Enter(adc_stream); break;

		case MSG_LEFT_CHANNEL:
			the_looper->RightChannel = false;
			the_looper->Stereo = false;
			break;
		case MSG_RIGHT_CHANNEL:
			the_looper->RightChannel = true;
			the_looper->Stereo = false;
			break;
		case MSG_STEREO_CHANNELS:
			the_looper->RightChannel = false;
			the_looper->Stereo = true;
			break;

		case MSG_TIME_DIV_100us: the_subscriber->SetTimePerDiv(0.1E-3); break;
		case MSG_TIME_DIV_200us: the_subscriber->SetTimePerDiv(0.2E-3); break;
		case MSG_TIME_DIV_500us: the_subscriber->SetTimePerDiv(0.5E-3); break;
		case MSG_TIME_DIV_1ms: the_subscriber->SetTimePerDiv(1E-3); break;
		case MSG_TIME_DIV_2ms: the_subscriber->SetTimePerDiv(2E-3); break;
		case MSG_TIME_DIV_5ms: the_subscriber->SetTimePerDiv(5E-3); break;
		case MSG_TIME_DIV_10ms: the_subscriber->SetTimePerDiv(10E-3); break;

		case MSG_TRIGGER_OFF: the_subscriber->SetTriggerMode(TRIGGER_OFF); break;
		case MSG_TRIGGER_LEVEL: the_subscriber->SetTriggerMode(TRIGGER_LEVEL); break;
		case MSG_TRIGGER_PEAK: the_subscriber->SetTriggerMode(TRIGGER_PEAK); break;

		case MSG_TRIGGER_LEFT: the_subscriber->TriggerRightChannel = false; break;
		case MSG_TRIGGER_RIGHT: the_subscriber->TriggerRightChannel = true; break;

		case MSG_SLOPE_POS: the_subscriber->TriggerSlopeNeg = false; break;
		case MSG_SLOPE_NEG: the_subscriber->TriggerSlopeNeg = true; break;

		case MSG_ILLUMINATION: {
			BScreen scr(this);
			illumination = !illumination;
			if (illumination) {
				c_black = scr.IndexForColor(128, 96, 0);
				c_dark_green = scr.IndexForColor(16, 32, 16);
			} else {
				c_black = scr.IndexForColor(0, 0, 0);
				c_dark_green = scr.IndexForColor(0, 32, 16);
			}
			break;
		}

		default:
			BWindow::MessageReceived(msg);
	}
}


/*
 *  Slider callbacks
 */

void QScopeWindow::trigger_level_callback(float value, void *arg)
{
	((QScopeWindow *)arg)->the_subscriber->TriggerLevel = (value - 0.5) * 65535;
}

void QScopeWindow::hold_off_callback(float value, void *arg)
{
	((QScopeWindow *)arg)->the_subscriber->SetHoldOff(value * 10.0);
}


/*
 *  Drawing looper constructor
 */

DrawLooper::DrawLooper(BitmapView *view, BBitmap *bitmap) : BLooper("QScope Drawing", B_DISPLAY_PRIORITY, 2)
{
	the_view = view;
	the_bounds = view->Bounds();
	the_window = view->Window();
	the_bitmap = bitmap;
	bits = (uint8 *)the_bitmap->Bits();
	xmod = the_bitmap->BytesPerRow();
	Stereo = false;
	RightChannel = false;
	Run();
}


/*
 *  Redraw oscilloscope
 */

void DrawLooper::MessageReceived(BMessage *msg)
{
	switch (msg->what) {
		case MSG_NEW_BUFFER: {	// New data buffer arrived, redraw oscilloscope
			int i, j;
			uint8 *p, *q, *r;
			int16 *buf;
			uint8 black = c_black;	// Local variables are faster
			uint8 *bits = this->bits;
			int xmod = this->xmod;

			// Prevent backlog
			BMessage *msg2;
			MessageQueue()->Lock();
			while ((msg2 = MessageQueue()->FindMessage(0L)) != NULL) {
				MessageQueue()->RemoveMessage(msg2);
				delete msg2;
			}
			MessageQueue()->Unlock();

			// Get pointer to data buffer
			if (msg->FindPointer("buffer", &buf) != B_NO_ERROR)
				break;

			// Draw dark green background
			memset(bits, c_dark_green, xmod * SCOPE_HEIGHT);

			// Draw data
			if (Stereo) {
				draw_data(buf, SCOPE_HEIGHT / 4, SCOPE_HEIGHT / 2);
				draw_data(buf + SCOPE_WIDTH * 2, SCOPE_HEIGHT * 3/4, SCOPE_HEIGHT / 2);
			} else
				if (RightChannel)
					draw_data(buf + SCOPE_WIDTH * 2, SCOPE_HEIGHT / 2, SCOPE_HEIGHT);
				else
					draw_data(buf, SCOPE_HEIGHT / 2, SCOPE_HEIGHT);

			// Draw grid and ticks
			for (i=0; i<NUM_Y_DIVS; i++) {
				memset(bits + xmod * (i * SCOPE_HEIGHT / NUM_Y_DIVS), black, SCOPE_WIDTH); 
				for (j=1; j<TICKS_PER_DIV; j++)
					memset(bits + SCOPE_WIDTH / 2 - 3 + xmod * (i * SCOPE_HEIGHT / NUM_Y_DIVS + j * SCOPE_HEIGHT / (NUM_Y_DIVS * TICKS_PER_DIV)), black, 7); 
			}
			memset(bits + xmod * (SCOPE_HEIGHT-1), black, SCOPE_WIDTH);

			p = bits;
			for (i=0; i<SCOPE_HEIGHT; i++) {
				for (j=0; j<NUM_X_DIVS; j++)
					p[j * SCOPE_WIDTH / NUM_X_DIVS] = black;
				p[SCOPE_WIDTH-1] = black;
				p += xmod;
			}
			p = bits + xmod * (SCOPE_HEIGHT / 2 - 3);
			q = bits + xmod * (SCOPE_HEIGHT / 4 - 2);
			r = bits + xmod * (SCOPE_HEIGHT * 3/4 - 2);
			for (i=0; i<NUM_X_DIVS; i++)
				for (j=1; j<TICKS_PER_DIV; j++) {
					int ofs = i * SCOPE_WIDTH / NUM_X_DIVS + j * SCOPE_WIDTH / (NUM_X_DIVS * TICKS_PER_DIV);
					p[ofs] = black;
					p[ofs + xmod] = black;
					p[ofs + xmod * 2] = black;
					p[ofs + xmod * 3] = black;
					p[ofs + xmod * 4] = black;
					p[ofs + xmod * 5] = black;
					p[ofs + xmod * 6] = black;
					q[ofs] = black;
					q[ofs + xmod] = black;
					q[ofs + xmod * 2] = black;
					q[ofs + xmod * 3] = black;
					q[ofs + xmod * 4] = black;
					r[ofs] = black;
					r[ofs + xmod] = black;
					r[ofs + xmod * 2] = black;
					r[ofs + xmod * 3] = black;
					r[ofs + xmod * 4] = black;
					bits[ofs + xmod * (SCOPE_HEIGHT * 3/16)] = black;
					bits[ofs + xmod * (SCOPE_HEIGHT * 13/16)] = black;
				}

			// Blit bitmap to screen
			if (the_window->LockWithTimeout(100000) == B_OK) {
				the_view->Draw(the_bounds);
				the_window->Unlock();
			}
			break;
		}

		default:
			BLooper::MessageReceived(msg);
	}
}


/*
 *  Draw oscilloscope beam
 */

void DrawLooper::draw_data(int16 *buf, int y_offset, int y_height)
{
	// y1 is top (maximum value), y2 is bottom (minimum value)
	int16 old_y1 = *buf++;
	int16 old_y2 = *buf++;
	int16 y1, y2;

	// Loop for all samples in buffer
	for (int i=0; i<SCOPE_WIDTH-1; i++, old_y1 = y1, old_y2 = y2) {

		// Get next sample values
		y1 = *buf++;
		y2 = *buf++;

		// Make sure that lines are connected
		if (y1 > old_y1 && y2 > old_y1)
			y2 = old_y1;
		if (y1 < old_y2 && y2 < old_y2)
			y1 = old_y2;

		uint32 y1_scr = y_offset - y1 * y_height / 65533;
		uint32 y2_scr = y_offset - y2 * y_height / 65533;

		if (y1_scr >= SCOPE_HEIGHT && y2_scr >= SCOPE_HEIGHT)
			continue;
		if (y1_scr >= SCOPE_HEIGHT)
			y1_scr = y1_scr & 0x80000000 ? SCOPE_HEIGHT-1 : 0;
		if (y2_scr >= SCOPE_HEIGHT)
			y2_scr = y2_scr & 0x80000000 ? SCOPE_HEIGHT-1 : 0;

		uint32 c_index = (y2_scr - y1_scr) * 16 / SCOPE_HEIGHT;
		if (c_index > 15)
			continue;
		int color = c_beam[c_index];
		uint8 *p = bits + xmod * y1_scr + i;
		for (int j=y1_scr; j<=y2_scr ;j++) {
			*p = color;
			p += xmod;
		}
	}
}


/*
 *  Subscriber constructor
 */

QScopeSubscriber::QScopeSubscriber(BLooper *looper) : BSubscriber("QScope")
{
	TriggerRightChannel = false;
	TriggerSlopeNeg = false;
	TriggerLevel = 0;
	SetTimePerDiv(2E-3);

	the_looper = looper;
	the_stream = NULL;

	state = STATE_RECORD;

	active_buf = 0;
	scope_counter = 0;
	record_counter = 0;
	next_frame = 0.0;
	old_input = 0;
	left_min = right_min = 32767;
	left_max = right_max = left_peak = right_peak = -32768;

	hold_off_counter = 0;
	SetHoldOff(0);

	trigger_start_frame = 0;
	trigger_total_frames = 0;
	SetTriggerMode(TRIGGER_LEVEL);
}


/*
 *  Subscriber destructor
 */

QScopeSubscriber::~QScopeSubscriber()
{
	if (the_stream != NULL) {
		ExitStream(true);
		Unsubscribe();
	}
}


/*
 *  Subscribe to audio stream
 */

void QScopeSubscriber::Enter(BAbstractBufferStream *stream)
{
	// Leave old stream
	if (the_stream != NULL) {
		ExitStream(true);
		Unsubscribe();
		the_stream = NULL;
	}

	// Subscribe to new stream
	if (Subscribe(stream) == B_NO_ERROR) {
		EnterStream(NULL, false, this, stream_func, NULL, true);
		the_stream = stream;
	}
}


/*
 *  Set time per division
 */

void QScopeSubscriber::SetTimePerDiv(float time)
{
	time_per_div = time;
	frame_add = time * SAMPLE_RATE * float(NUM_X_DIVS) / float(SCOPE_WIDTH);
	SetHoldOff(hold_off);
}


/*
 *  Set hold-off time
 */

void QScopeSubscriber::SetHoldOff(float hold)
{
	hold_off = hold;
	hold_off_frames = hold * time_per_div * SAMPLE_RATE;
}


/*
 *  Set trigger mode
 */

void QScopeSubscriber::SetTriggerMode(int mode)
{
	trigger_mode = mode;
}


/*
 *  Stream function
 */

bool QScopeSubscriber::stream_func(void *arg, char *buf, size_t count, void *header)
{
	((QScopeSubscriber *)arg)->scope_func((int16 *)buf, count);
	return true;
}

void QScopeSubscriber::scope_func(int16 *buf, size_t count)
{
	// Number of sample frames in input buffer
	count >>= 2;

	// Act according to current state
	switch (state) {
		case STATE_HOLD_OFF:	// Wait before next trigger
hold_off:
			if (hold_off_counter >= count) {

				// Still waiting
				hold_off_counter -= count;

			} else {

				// Time elapsed, now search for trigger level (or start recording if not triggered)
				if (trigger_mode != TRIGGER_OFF) {
					state = STATE_WAIT_FOR_TRIGGER;
					trigger_start_frame = hold_off_counter;
					trigger_total_frames = 0;
					goto wait_for_trigger;
				} else {
					state = STATE_RECORD;
					next_frame = float(hold_off_counter);
					goto record;
				}
			}
			break;

		case STATE_WAIT_FOR_TRIGGER: {	// Search for trigger level
wait_for_trigger:
			int i;
			int16 input;
			if (trigger_mode == TRIGGER_PEAK) {
				if (TriggerRightChannel) {
					int16 compare = right_peak - 256;
					for (i=trigger_start_frame; i<count; i++) {
						input = buf[(i << 1) + 1];
						if (input >= compare)
							goto trigger_found;
					}
				} else {
					int16 compare = left_peak - 256;
					for (i=trigger_start_frame; i<count; i++) {
						input = buf[i << 1];
						if (input >= compare)
							goto trigger_found;
						old_input = input;
					}
				}
			} else {
				bool trigger_right = TriggerRightChannel;
				int trigger_level = TriggerLevel;
				if (TriggerSlopeNeg) {
					for (i=trigger_start_frame; i<count; i++) {
						input = buf[(i << 1) + trigger_right];
						if (input < trigger_level && old_input > trigger_level)
							goto trigger_found;
						old_input = input;
					}
				} else {
					for (i=trigger_start_frame; i<count; i++) {
						input = buf[(i << 1) + trigger_right];
						if (input > trigger_level && old_input < trigger_level)
							goto trigger_found;
						old_input = input;
					}
				}
			}
			trigger_start_frame = 0;

			// Trigger anyway if we have waited more than 1/30s
			trigger_total_frames += count;
			if (trigger_total_frames > SAMPLE_RATE / 30)
				goto trigger_found;
			break;

trigger_found:
			state = STATE_RECORD;
			record_counter = i;
			next_frame = float(i) + frame_add;
			left_min = left_max = buf[i << 1];
			right_min = right_max = buf[(i << 1) + 1];
			left_peak = right_peak = -32768;
			old_input = input;
			goto record;
		}

		case STATE_RECORD: {	// Get samples and stuff them into scope_buf
record:
			int next = int(next_frame);
			bool reaches_next_frame = true;
			if (next > count) {
				next = count;
				reaches_next_frame = false;
			}

			// Search minimum and maximum
			for (int i=record_counter; i<next; i++) {
				int16 left = buf[i << 1];
				int16 right = buf[(i << 1) + 1];
				if (left < left_min)
					left_min = left;
				if (left > left_max) {
					left_max = left;
					if (left > left_peak)
						left_peak = left;
				}
				if (right < right_min)
					right_min = right;
				if (right > right_max) {
					right_max = right;
					if (right > right_peak)
						right_peak = right;
				}
			}

			if (reaches_next_frame) {

				// Record one sample
				scope_buf[active_buf][scope_counter] = left_max;
				scope_buf[active_buf][scope_counter + SCOPE_WIDTH * 2] = right_max;
				scope_counter++;
				scope_buf[active_buf][scope_counter] = left_min;
				scope_buf[active_buf][scope_counter + SCOPE_WIDTH * 2] = right_min;
				scope_counter++;

				// scope_buf full? Then send message to looper
				if (scope_counter == SCOPE_WIDTH * 2) {
					scope_counter = 0;

					BMessage msg(MSG_NEW_BUFFER);
					msg.AddPointer("buffer", scope_buf[active_buf]);
					the_looper->PostMessage(&msg);
					active_buf = !active_buf;

					state = STATE_HOLD_OFF;
					hold_off_counter = hold_off_frames + int(next_frame);
					goto hold_off;
				}

				// Advance to next frame
				record_counter = next;
				next_frame += frame_add;
				if (record_counter < count) {
					left_min = left_max = buf[record_counter << 1];
					right_min = right_max = buf[(record_counter << 1) + 1];
					goto record;
				} else {

					// Input buffer used up
					left_min = left_max = buf[(count-1) << 1];
					right_min = right_max = buf[((count-1) << 1) + 1];
					record_counter = 0;
					next_frame -= count;
				}

			} else {

				// Input buffer used up
				record_counter = 0;
				next_frame -= count;
			}
			break;
		}
	}
}
