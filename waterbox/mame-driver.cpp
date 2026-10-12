// chimera-core-mame: the arcade machines of upstream MAME as a Chimera core.
//
// MAME is built the way it builds a chosen set of machines (build-mame.sh),
// and this file is the rest of what an emulator needs around it: MAME's
// frontend and its OSD layer are replaced by
//
//  * a machine_manager whose UI does nothing - no menus, no startup text, no
//    warning screens, no Lua, no plugins;
//  * an osd_interface that hands each frame's picture (MAME's own software
//    renderer, drawing the machine's layout) and each frame's sound (one
//    stereo stream) to the sandbox;
//  * the panel: every control MAME knows by type and player, written straight
//    into the fields of the machine's ports each frame - no input devices,
//    no key bindings, no smoothing of an analog control;
//  * the database: MAME's own driver table says which files a game needs, and
//    MAME's own audit says which of them are missing or wrong, before a
//    machine is built.
//
// MAME runs its machine as a loop that returns only when the machine exits.
// That loop runs on a cothread of its own, and the OSD's per-frame update()
// - which MAME calls at every VBLANK of the screen - switches back to the
// caller of mame_frame(): a frame is exactly one turn of MAME's own loop, and
// nothing in it is reordered.

#include "emu.h"
#include "emuopts.h"
#include "input.h"
#include "inputdev.h"
#include "frontend/mame/ui/menuitem.h"
#include "frontend/mame/audit.h"
#include "main.h"
#include "render.h"
#include "rendersw.hxx"
#include "drivenum.h"
#include "romload.h"
#include "screen.h"
#include "dinvram.h"
#include "ioprocs.h"
#include "ioprocsvec.h"
#include "sound.h"
#include "video.h"
#include "ui/uimain.h"
#include "osdepend.h"
#include "modules/lib/osdobj_common.h"

#include "libco/libco.h"
#include "mame-driver.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

constexpr int kSampleRate = 48000;
constexpr int kMaxSide = 2048;     // waterbox.config, video

std::string s_game;
std::map<std::string, std::string> s_options;
std::map<std::string, std::string> s_switches;
std::string s_error;
std::string s_log;          // what MAME printed as errors and warnings
running_machine *s_machine;

// why the machine may not start, found while MAME was starting it. MAME
// catches what is thrown there and carries on, so it is kept and mame_init
// answers with it.
std::string s_refused;

// ---- the panel -----------------------------------------------------------
// One entry per control MAME knows: a type and a player ("P1 Button 1",
// "Coin 2"). The list is MAME's own (ioport_manager::types), so it is the
// same for every game; what differs is which entries a game's ports have.
// Controls a driver names itself, with no type of their own (keypads,
// keyboards, odd switches), take the numbered places after it.
struct control
{
	std::string token;      // MAME's own name for the type and player: P1_BUTTON1
	std::string name;       // MAME's default label: "P1 Button 1"
	ioport_type type;
	int player;
	std::string label;      // what this game's driver calls it, or ""
	std::vector<ioport_field *> fields;
	// an axis: its rest and its two ends as the game reads them (a control
	// whose range is signed has a negative end), and the bits of its field
	s32 rest = 0, min = 0, max = 0;
	u32 bits = 0;
	bool relative = false;  // a dial or a ball: the axis is this frame's movement
	s32 position = 0;       // where a relative control has got to
};
std::vector<control> s_buttons, s_axes;
std::vector<int32_t> s_button_state, s_axis_state;
int s_first_other;
bool s_input_read;
// the ports that hold at least one of the game's controls: a read of one of
// these is the game looking at its controls (a port of dip switches is not)
std::set<ioport_port *> s_control_ports;
// a machine that is TOLD when a control changes (a callback on the field, as
// Pong's circuit has) never reads a port for it: such a machine has no frame
// in which it "did not read its controls", and none is counted
bool s_controls_pushed;

// a control with no type of its own
bool is_nameless(ioport_type type)
{
	return type == IPT_OTHER || type == IPT_KEYPAD || type == IPT_KEYBOARD;
}

// The controls the package declares (waterbox/panel.txt, written by
// gen-config.py from a walk over every machine: the controls at least one
// machine has). The panel is these, in MAME's order, so a control's index is
// its place in the package's list. With no list, the panel is every control
// MAME knows - which is what the walk itself runs with.
#include "panel-list.inc"

// every control MAME knows, in MAME's order
void all_controls(running_machine &machine, std::vector<control> &buttons, std::vector<control> &axes)
{
	for (input_type_entry const &entry : machine.ioport().types())
	{
		// MAME's own keys (pause, menus, save state) are not the machine's
		if (entry.group() == IPG_UI || entry.group() == IPG_INVALID) continue;
		if (is_nameless(entry.type())) continue;
		if (!entry.token() || !*entry.token()) continue;
		control c;
		c.token = entry.token();
		c.name = entry.name();
		c.type = entry.type();
		c.player = entry.player();
		(ioport_manager::type_is_analog(entry.type()) ? axes : buttons).push_back(std::move(c));
	}
}

void build_panel(running_machine &machine)
{
	s_buttons.clear();
	s_axes.clear();
	std::vector<control> buttons, axes;
	all_controls(machine, buttons, axes);
	std::set<std::string> const listed(std::begin(kPanelList), std::end(kPanelList) - 1);
	for (control &c : buttons)
		if (listed.empty() || listed.count(c.token)) s_buttons.push_back(std::move(c));
	for (control &c : axes)
		if (listed.empty() || listed.count(c.token)) s_axes.push_back(std::move(c));
	s_first_other = int(s_buttons.size());
	for (int i = 0; i < kPanelOthers; i++)
	{
		control c;
		c.token = "OTHER" + std::to_string(i + 1);
		c.name = "Other " + std::to_string(i + 1);
		c.type = IPT_OTHER;
		c.player = 0;
		s_buttons.push_back(std::move(c));
	}
	s_button_state.assign(s_buttons.size(), 0);
	s_axis_state.assign(s_axes.size(), 0);
}

// the lowest set bit of a field's mask: where its value starts in the port
int field_shift(ioport_field const &field)
{
	int shift = 0;
	for (ioport_value mask = field.mask(); mask && !(mask & 1); mask >>= 1) shift++;
	return shift;
}

// An analog field as the frontend drives it: where it rests, how far it goes
// each way, and whether the axis is a position or a movement. MAME's own
// reading of the same field is analog_field's constructor (ioport.cpp).
struct analog_shape { s32 rest, min, max; u32 bits; bool relative; };
analog_shape shape_of(ioport_field const &field)
{
	int const shift = field_shift(field);
	analog_shape a;
	a.bits = u32(field.mask() >> shift);
	a.rest = s32((field.defvalue() & field.mask()) >> shift);
	a.min = s32((field.minval() & field.mask()) >> shift);
	a.max = s32((field.maxval() & field.mask()) >> shift);
	a.relative = field.type() == IPT_DIAL || field.type() == IPT_DIAL_V
		|| field.type() == IPT_TRACKBALL_X || field.type() == IPT_TRACKBALL_Y
		|| field.type() == IPT_MOUSE_X || field.type() == IPT_MOUSE_Y;
	if (a.relative)
	{
		// a counter the width of the field, going round
		a.min = 0;
		a.max = s32(a.bits);
	}
	else if (field.type() == IPT_POSITIONAL || field.type() == IPT_POSITIONAL_V)
	{
		// one of maxval positions
		a.min = 0;
		a.max = std::max<s32>(s32(field.maxval()) - 1, 0);
	}
	else if (a.min > a.max)
	{
		// written as signed: 0x80 to 0x7f is -128 to 127
		a.min -= s32(a.bits) + 1;
		if (a.rest > a.max) a.rest -= s32(a.bits) + 1;
	}
	return a;
}

// Finds the game's controls among the panel's, and takes them off MAME's own
// key bindings: what a field reads is what mame_set_button and mame_set_axis
// wrote, and nothing else.
void bind_panel(running_machine &machine)
{
	std::map<std::pair<int, int>, int> button_at, axis_at;
	for (int i = 0; i < s_first_other; i++) button_at[{ int(s_buttons[i].type), s_buttons[i].player }] = i;
	for (int i = 0; i < int(s_axes.size()); i++) axis_at[{ int(s_axes[i].type), s_axes[i].player }] = i;
	int other = s_first_other;
	for (auto &port : machine.ioport().ports())
	{
		for (ioport_field &field : port.second->fields())
		{
			if (field.type_class() != INPUT_CLASS_CONTROLLER && field.type_class() != INPUT_CLASS_KEYBOARD
				&& field.type_class() != INPUT_CLASS_MISC) continue;
			if (field.type() == IPT_UNUSED || field.type() == IPT_UNKNOWN || field.type() == IPT_SPECIAL
				|| field.type() == IPT_CUSTOM || field.type() == IPT_OUTPUT) continue;
			ioport_field::user_settings us;
			field.get_user_settings(us);
			for (auto &seq : us.seq) seq = input_seq();
			field.set_user_settings(us);

			control *c = nullptr;
			if (field.is_analog())
			{
				auto const at = axis_at.find({ int(field.type()), int(field.player()) });
				if (at == axis_at.end()) continue;
				c = &s_axes[at->second];
				if (c->fields.empty())
				{
					analog_shape const a = shape_of(field);
					c->rest = a.rest;
					c->min = a.min;
					c->max = a.max;
					c->bits = a.bits;
					c->relative = a.relative;
					c->position = a.rest;
				}
			}
			else if (is_nameless(field.type()))
			{
				if (other >= int(s_buttons.size())) continue;
				c = &s_buttons[other++];
			}
			else
			{
				auto const at = button_at.find({ int(field.type()), int(field.player()) });
				if (at == button_at.end()) continue;
				c = &s_buttons[at->second];
			}
			if (c->fields.empty() && field.specific_name())
				c->label = field.specific_name();
			c->fields.push_back(&field);
			s_control_ports.insert(port.second.get());
			if (field.has_dynamic_write()) s_controls_pushed = true;
		}
	}
}

// this frame's controls, into the ports
void apply_panel()
{
	for (size_t i = 0; i < s_buttons.size(); i++)
		for (ioport_field *field : s_buttons[i].fields)
			field->set_value(s_button_state[i] ? 1 : 0);
	for (size_t i = 0; i < s_axes.size(); i++)
	{
		control &c = s_axes[i];
		if (c.fields.empty()) continue;
		s32 value;
		if (c.relative)
		{
			// a dial has no ends: its counter goes round
			c.position = s32(u32(c.position + s_axis_state[i]) & c.bits);
			value = c.position;
		}
		else
			value = std::clamp(c.rest + s_axis_state[i], c.min, c.max);
		// the port's bits (a negative value in two's complement)
		for (ioport_field *field : c.fields)
			field->set_value(ioport_value(u32(value) & c.bits));
	}
}

// ---- the game's own switches ---------------------------------------------
// A dip switch or a machine configuration switch, by the driver's name for it
// and for the position.
void apply_switches(running_machine &machine)
{
	std::set<std::string> done;
	for (auto &port : machine.ioport().ports())
	{
		for (ioport_field &field : port.second->fields())
		{
			if (field.type() != IPT_DIPSWITCH && field.type() != IPT_CONFIG) continue;
			if (!field.specific_name()) continue;
			auto const want = s_switches.find(field.specific_name());
			if (want == s_switches.end()) continue;
			for (ioport_setting const &setting : field.settings())
			{
				if (want->second != setting.name()) continue;
				ioport_field::user_settings us;
				field.get_user_settings(us);
				us.value = setting.value();
				field.set_user_settings(us);
				done.insert(want->first);
				break;
			}
		}
	}
	for (auto const &kv : s_switches)
		if (!done.count(kv.first))
			s_refused = "this game has no switch \"" + kv.first + "\" with a position \"" + kv.second + "\"";
}

// ---- what the machine keeps with the power off -----------------------------
// Every device of the machine that keeps something (an EEPROM, a battery-
// backed RAM, a timekeeper chip) is one save data file, in the form MAME
// itself writes to its nvram folder, named by the device: ":eeprom" is
// eeprom.nv. MAME's own nvram files are neither read nor written.
std::map<std::string, std::vector<uint8_t>> s_saves_in;
struct save_file { std::string name; std::vector<uint8_t> bytes; };
std::vector<save_file> s_saves_out;

std::string save_name(device_t const &device)
{
	std::string name = device.tag() + 1;
	std::replace(name.begin(), name.end(), ':', '_');
	return name + ".nv";
}

// puts the files given back into their devices: after MAME has set every
// device to its factory contents, before the machine is reset
void load_saves(running_machine &machine)
{
	std::set<std::string> used;
	std::string have;
	for (device_nvram_interface &nvram : nvram_interface_enumerator(machine.root_device()))
	{
		std::string const name = save_name(nvram.device());
		have += (have.empty() ? "" : ", ") + name;
		auto const given = s_saves_in.find(name);
		if (given == s_saves_in.end()) continue;
		auto file = util::ram_read(given->second.data(), given->second.size());
		if (!file || !nvram.nvram_load(*file))
			s_refused = "the save data " + name + " could not be loaded into this machine";
		used.insert(name);
	}
	for (auto const &given : s_saves_in)
		if (!used.count(given.first))
			s_refused = "the save data " + given.first + " is not one of this machine's ("
				+ (have.empty() ? std::string("it keeps nothing") : "it keeps " + have) + ")";
}

// ---- what one frame leaves behind ---------------------------------------
std::vector<uint32_t> s_video;
int s_width, s_height;
int s_aspect_x = 4, s_aspect_y = 3;
std::vector<int16_t> s_audio;

cothread_t s_host, s_mame;
bool s_stopped;

// "(4:3)" at the end of a view's name
bool view_aspect(const char *name, int &x, int &y)
{
	const char *open = std::strrchr(name, '(');
	int a, b;
	if (!open || std::sscanf(open, "(%d:%d)", &a, &b) != 2 || a <= 0 || b <= 0) return false;
	x = a;
	y = b;
	return true;
}

// ---- the OSD --------------------------------------------------------------
class chimera_osd final : public osd_interface
{
public:
	void init(running_machine &machine) override
	{
		m_machine = &machine;
		m_target = machine.render().target_alloc();
		// A machine with one screen is drawn as that screen's own pixels: its
		// "Pixel Aspect" view. The "Standard" view beside it carries the shape
		// the monitor showed them in. Anything else (several screens, a
		// cabinet layout) is drawn as MAME's first view.
		int pixel = -1;
		for (int v = 0; m_target->view_name(v); v++)
		{
			const char *name = m_target->view_name(v);
			if (pixel < 0 && std::strstr(name, "Pixel Aspect")) pixel = v;
			if (std::strstr(name, "Standard")) view_aspect(name, s_aspect_x, s_aspect_y);
		}
		bool const one_screen = screen_device_enumerator(machine.root_device()).count() == 1;
		m_native = one_screen && pixel >= 0;
		if (m_native)
		{
			m_target->set_view(pixel);
			// the picture is the screen's pixels and nothing else: draw() sizes
			// the target to them every frame, whatever shape the view was made in
			m_target->set_keepaspect(false);
		}
	}

	void update(bool skip_redraw) override
	{
		// this frame's sound, all of it (patch 0003): MAME's own periodic update
		// runs at 50 Hz, not at the screen's rate
		m_machine->sound().chimera_flush();
		if (!skip_redraw) draw();
		// the end of a frame: back to mame_frame's caller
		co_switch(s_host);
	}

	void input_update(bool relative_reset) override { }
	void check_osd_inputs() override { }
	void set_verbose(bool print_verbose) override { }
	void init_debugger() override { }
	void wait_for_debugger(device_t &device, bool firststop) override { }

	bool no_sound() override { return false; }
	bool sound_external_per_channel_volume() override { return false; }
	bool sound_split_streams_per_source() override { return false; }
	uint32_t sound_get_generation() override { return 1; }
	osd::audio_info sound_get_information() override
	{
		osd::audio_info result;
		result.m_generation = 1;
		result.m_default_sink = 1;
		result.m_default_source = 0;
		result.m_nodes.resize(1);
		auto &node = result.m_nodes[0];
		node.m_name = "chimera";
		node.m_display_name = "Chimera";
		node.m_id = 1;
		node.m_rate.m_default_rate = kSampleRate;
		node.m_rate.m_min_rate = kSampleRate;
		node.m_rate.m_max_rate = kSampleRate;
		node.m_sinks = 2;
		node.m_sources = 0;
		node.m_port_names = { "L", "R" };
		node.m_port_positions = { osd::channel_position::FL(), osd::channel_position::FR() };
		if (m_stream)
			result.m_streams.emplace_back(m_stream, 1u);
		return result;
	}
	uint32_t sound_stream_sink_open(uint32_t node, std::string name, uint32_t rate) override { return m_stream = 1; }
	uint32_t sound_stream_source_open(uint32_t node, std::string name, uint32_t rate) override { return 0; }
	void sound_stream_close(uint32_t id) override { if (id == m_stream) m_stream = 0; }
	void sound_stream_sink_update(uint32_t id, const int16_t *buffer, int samples_this_frame) override
	{
		s_audio.insert(s_audio.end(), buffer, buffer + 2 * samples_this_frame);
	}
	void sound_stream_source_update(uint32_t id, int16_t *buffer, int samples_this_frame) override { }
	void sound_stream_set_volumes(uint32_t id, const std::vector<float> &db) override { }
	void sound_begin_update() override { }
	void sound_end_update() override { }

	void customize_input_type_list(std::vector<input_type_entry> &typelist) override { }
	void add_audio_to_recording(const int16_t *buffer, int samples_this_frame) override { }
	std::vector<ui::menu_item> get_slider_list() override { return {}; }
	osd_font::ptr font_alloc() override { return nullptr; }
	bool get_font_families(std::string const &font_path, std::vector<std::pair<std::string, std::string>> &result) override { return false; }
	bool execute_command(const char *command) override { return false; }
	std::unique_ptr<osd::midi_input_port> create_midi_input(std::string_view name) override { return nullptr; }
	std::unique_ptr<osd::midi_output_port> create_midi_output(std::string_view name) override { return nullptr; }
	std::vector<osd::midi_port_info> list_midi_ports() override { return {}; }
	std::unique_ptr<osd::network_device> open_network_device(int id, osd::network_handler &handler) override { return nullptr; }
	std::vector<osd::network_device_info> list_network_devices() override { return {}; }

private:
	void draw()
	{
		s32 w = 0, h = 0;
		m_target->compute_minimum_size(w, h);
		if (w <= 0 || h <= 0) return;
		// the package's picture buffer is kMaxSide square: a layout larger
		// than that is drawn smaller, in the same shape
		while (w > kMaxSide || h > kMaxSide) { w = (w + 1) / 2; h = (h + 1) / 2; }
		m_target->set_bounds(w, h);
		s_video.assign(size_t(w) * h, 0);
		s_width = w;
		s_height = h;
		if (!m_native)
		{
			// a layout is drawn in its own shape
			s_aspect_x = w;
			s_aspect_y = h;
		}
		render_primitive_list &prims = m_target->get_primitives();
		prims.acquire_lock();
		software_renderer<uint32_t, 0, 0, 0, 16, 8, 0>::draw_primitives(prims, s_video.data(), w, h, w);
		prims.release_lock();
	}

	running_machine *m_machine = nullptr;
	render_target *m_target = nullptr;
	bool m_native = false;
	uint32_t m_stream = 0;
};

// ---- the frontend ---------------------------------------------------------
class chimera_manager final : public machine_manager
{
public:
	chimera_manager(emu_options &options, osd_interface &osd) : machine_manager(options, osd) { }
	// a UI that shows nothing: no menus, no startup text, no warning screens
	ui_manager *create_ui(running_machine &machine) override { return m_ui = new ui_manager(machine); }
	// the ports exist now, and no saved settings are read after this (readconfig off)
	void before_load_settings(running_machine &machine) override
	{
		build_panel(machine);
		bind_panel(machine);
		apply_switches(machine);
		// Unthrottled. The "throttle" option is read by MAME's frontend, not by
		// its emulator core, which otherwise paces the machine to the host's
		// clock - sleeping between frames, and deciding by the host's time.
		machine.video().set_throttled(false);
	}
	// MAME has given every device its factory contents (its own nvram files
	// are off) and set the clocks; the machine is not reset yet, so what is
	// loaded here is what it powers on with
	void ui_initialize(running_machine &machine) override { load_saves(machine); }
	~chimera_manager() override { delete m_ui; }
private:
	ui_manager *m_ui = nullptr;
};

chimera_osd *s_osd;
emu_options *s_emu_options;
chimera_manager *s_manager;

void mame_main()
{
	try
	{
		machine_config config(*s_emu_options->system(), *s_emu_options);
		running_machine machine(config, *s_manager);
		s_manager->set_machine(&machine);
		s_machine = &machine;
		int const result = machine.run(false);
		if (s_error.empty())
			s_error = "the machine stopped (MAME exit code " + std::to_string(result) + ")";
	}
	catch (emu_fatalerror const &e)
	{
		s_error = e.what();
	}
	catch (std::exception const &e)
	{
		s_error = e.what();
	}
	if (!s_log.empty())
		s_error += "\n" + s_log;
	s_machine = nullptr;
	s_stopped = true;
	for (;;) co_switch(s_host);
}

void set(core_options &opts, const char *name, std::string const &value)
{
	if (!opts.get_entry(name))
		throw std::runtime_error(std::string("MAME has no option \"") + name + "\"");
	opts.set_value(name, value, OPTION_PRIORITY_CMDLINE);
}

// the options every machine of this core runs with: nothing may depend on the host
void fixed_options(emu_options &o)
{
	set(o, OPTION_THROTTLE, "0");
	set(o, OPTION_SLEEP, "0");
	set(o, OPTION_SAMPLERATE, std::to_string(kSampleRate));
	set(o, OPTION_RTC_TIME, "20000101000000");
	set(o, OPTION_NVRAM_SAVE, "0");
	set(o, OPTION_READCONFIG, "0");
	set(o, OPTION_WRITECONFIG, "0");
	// where the rom sets are: Chimera mounts a project's files in the
	// sandbox's root ("/ssf2t.zip"); the test harnesses run in a work folder
	set(o, OPTION_MEDIAPATH, "/;.");
	set(o, OPTION_SAMPLEPATH, "/;.");
	// both ways of a stick at once reach the game as they are: what a game
	// does with them is the game's
	set(o, OPTION_JOYSTICK_CONTRADICTORY, "1");
}

std::string json(std::string_view s)
{
	std::string out = "\"";
	for (unsigned char ch : s)
	{
		if (ch == '"' || ch == '\\') { out += '\\'; out += char(ch); }
		else if (ch == '\n') out += "\\n";
		else if (ch == '\t') out += "\\t";
		else if (ch < 0x20) out += util::string_format("\\u%04x", unsigned(ch));
		else out += char(ch);
	}
	return out + "\"";
}

bool file_exists(std::string const &name)
{
	FILE *const f = std::fopen(name.c_str(), "rb");
	if (!f) return false;
	std::fclose(f);
	return true;
}

// is this set among the files given: name.zip or name.7z
bool set_given(std::string const &name)
{
	return file_exists(name + ".zip") || file_exists(name + ".7z")
		|| file_exists("/" + name + ".zip") || file_exists("/" + name + ".7z");
}

// what MAME says of how well a machine is emulated
std::string status_text(game_driver const &driver, machine_config const &config)
{
	std::vector<std::string> parts;
	if (driver.type.emulation_flags() & device_t::flags::NOT_WORKING) parts.emplace_back("MAME marks this machine as NOT WORKING");
	if (driver.flags & MACHINE_MECHANICAL) parts.emplace_back("the machine has mechanical parts MAME does not show");
	device_t::feature_type unemulated = device_t::feature::NONE, imperfect = device_t::feature::NONE;
	for (device_t &device : device_enumerator(config.root_device()))
	{
		unemulated |= device.type().unemulated_features();
		imperfect |= device.type().imperfect_features();
	}
	struct { device_t::feature_type bit; const char *name; } const names[] = {
		{ device_t::feature::PROTECTION, "protection" }, { device_t::feature::GRAPHICS, "graphics" },
		{ device_t::feature::PALETTE, "colours" }, { device_t::feature::SOUND, "sound" },
		{ device_t::feature::TIMING, "timing" }, { device_t::feature::CONTROLS, "controls" },
	};
	for (auto const &n : names)
	{
		if (unemulated & n.bit) parts.push_back(std::string(n.name) + " not emulated");
		else if (imperfect & n.bit) parts.push_back(std::string(n.name) + " imperfect");
	}
	std::string out;
	for (auto const &p : parts) out += (out.empty() ? "" : "; ") + p;
	return out;
}

} // anonymous namespace

// called by MAME at every read of a port (patch 0005)
void chimera_port_read(ioport_port &port)
{
	if (!s_input_read && s_control_ports.count(&port)) s_input_read = true;
}

// MAME's output (errors, warnings) goes to stderr - it is the core's log - and
// is kept, so a machine that will not start can say why
class chimera_output : public osd_output
{
public:
	void output_callback(osd_output_channel channel, util::format_argument_pack<char> const &args) override
	{
		if (channel == OSD_OUTPUT_CHANNEL_ERROR || channel == OSD_OUTPUT_CHANNEL_WARNING)
		{
			std::string const text = util::string_format(args);
			std::fputs(text.c_str(), stderr);
			if (s_log.size() < 16384) s_log += text;
		}
	}
};

static void push_output()
{
	static chimera_output output;
	static bool pushed;
	if (!pushed) osd_output::push(&output);
	pushed = true;
}

extern "C" int mame_set_game(const char *name)
{
	push_output();
	s_game = name;
	if (driver_list::find(name) >= 0) return 0;
	s_error = "MAME has no machine named \"" + s_game + "\".";
	try
	{
		emu_options options;
		driver_enumerator drivers(options);
		int close[5];
		drivers.find_approximate_matches(s_game, 5, close);
		std::string names;
		for (int index : close)
			if (index >= 0)
				names += (names.empty() ? "" : ", ") + std::string(driver_list::driver(index).name)
					+ " (" + driver_list::driver(index).type.fullname() + ")";
		if (!names.empty()) s_error += " The closest names: " + names + ".";
	}
	catch (std::exception const &) { }
	s_error += " A game is known by the name of its file: ssf2t.zip is the machine ssf2t.";
	return 1;
}

extern "C" void mame_set_option(const char *name, const char *value) { s_options[name] = value; }
extern "C" void mame_set_switch(const char *name, const char *value) { s_switches[name] = value; }

// ---- the database --------------------------------------------------------
namespace {

struct needed_file
{
	std::string name;       // the file, as MAME names it
	std::string set;        // the set it belongs in
	bool disc;
	bool required;          // has a known good dump, and is not optional
	std::string problem;    // "", or what is wrong with it
};

// every file the machine reads, with what MAME's audit found for it
std::vector<needed_file> audit(emu_options &options, game_driver const &driver, machine_config &config)
{
	// what each ancestor's own set holds: a file of the game that a parent
	// has too is kept in the parent's set
	std::vector<std::pair<std::string, std::vector<rom_entry>>> ancestors;
	for (int at = driver_list::find(driver.parent); at >= 0; at = driver_list::find(driver_list::driver(at).parent))
		ancestors.emplace_back(driver_list::driver(at).name, rom_build_entries(driver_list::driver(at).rom));

	// the BIOS the machine starts with: the one asked for, or MAME's default
	u32 bios = config.root_device().default_bios();
	if (auto const asked = s_options.find("bios"); asked != s_options.end())
		for (romload::system_bios const &choice : romload::entries(driver.rom).get_system_bioses())
			if (asked->second == choice.get_name()) bios = choice.get_value();

	std::vector<needed_file> out;
	driver_enumerator drivers(options, driver);
	drivers.next();
	media_auditor auditor(drivers);
	for (device_t &device : device_enumerator(config.root_device()))
	{
		std::vector<needed_file> files;
		for (rom_entry const *region = rom_first_region(device); region; region = rom_next_region(region))
		{
			if (!ROMREGION_ISROMDATA(region) && !ROMREGION_ISDISKDATA(region)) continue;
			for (rom_entry const *rom = rom_first_file(region); rom; rom = rom_next_file(rom))
			{
				util::hash_collection const hashes(rom->hashdata());
				// a file of a BIOS the machine will not start with is not read
				if (ROM_GETBIOSFLAGS(rom) != 0 && ROM_GETBIOSFLAGS(rom) != (&device == &config.root_device() ? bios : device.default_bios()))
					continue;
				needed_file f;
				f.name = rom->name();
				f.disc = ROMREGION_ISDISKDATA(region);
				f.required = !hashes.flag(util::hash_collection::FLAG_NO_DUMP) && !ROM_ISOPTIONAL(rom);
				f.set = device.shortname();
				if (&device == &config.root_device())
				{
					for (auto const &ancestor : ancestors)
						for (rom_entry const &theirs : ancestor.second)
							if (ROMENTRY_ISFILE(theirs) && util::hash_collection(theirs.hashdata()) == hashes
								&& (f.required || theirs.name() == f.name))
								f.set = ancestor.first;
				}
				files.push_back(std::move(f));
			}
		}
		if (files.empty()) continue;
		auditor.audit_device(device, AUDIT_VALIDATE_FAST);
		// MAME's audit has one record for each file of the device (the other
		// BIOSes' too), in order - or none at all when it found no file of it
		auto const &records = auditor.records();
		for (needed_file &f : files)
		{
			auto record = records.begin();
			for (; record != records.end(); ++record)
				if (record->name() == f.name && (record->type() == media_auditor::media_type::DISK) == f.disc) break;
			if (record == records.end())
			{
				if (f.required) f.problem = "missing";
			}
			else
			{
				switch (record->substatus())
				{
				case media_auditor::audit_substatus::NOT_FOUND: f.problem = "missing"; break;
				case media_auditor::audit_substatus::FOUND_BAD_CHECKSUM: f.problem = "found, but its contents are not the ones MAME expects"; break;
				case media_auditor::audit_substatus::FOUND_WRONG_LENGTH:
					f.problem = util::string_format("found, but it is %d bytes and MAME expects %d", record->actual_length(), record->expected_length());
					break;
				default: break;
				}
			}
			out.push_back(std::move(f));
		}
	}
	return out;
}

std::string s_description;

// what mame_describe found, for the functions that hand it out in pieces
struct described_switch { std::string name, kind, chosen; std::vector<std::string> options; };
struct described
{
	std::string error, title, status;
	std::vector<std::pair<std::string, bool>> sets;      // name, given
	size_t files = 0;
	std::vector<needed_file> problems;
	std::vector<std::pair<std::string, std::string>> bios;   // name, description
	std::string default_bios;
	std::vector<described_switch> switches;
};
described s_desc;

} // anonymous namespace

extern "C" const char *mame_describe(void)
{
	push_output();
	try
	{
		int const index = driver_list::find(s_game.c_str());
		if (index < 0) throw std::runtime_error(s_error);
		game_driver const &driver = driver_list::driver(index);
		emu_options options;
		fixed_options(options);
		options.set_system_name(s_game);
		machine_config config(driver, options);
		s_desc = described();
		s_desc.title = util::string_format("%s (%s, %s)", driver.type.fullname(), driver.year, driver.manufacturer);
		s_desc.status = status_text(driver, config);

		std::string out = "{\"game\":" + json(driver.name) + ",\"name\":" + json(driver.type.fullname())
			+ ",\"year\":" + json(driver.year) + ",\"maker\":" + json(driver.manufacturer)
			+ ",\"parent\":" + json(driver_list::find(driver.parent) >= 0 ? driver.parent : "")
			+ ",\"status\":" + json(status_text(driver, config));

		// the files
		std::vector<needed_file> const files = audit(options, driver, config);
		std::vector<std::string> sets;
		for (needed_file const &f : files)
			if (!f.disc && f.required && std::find(sets.begin(), sets.end(), f.set) == sets.end())
				sets.push_back(f.set);
		for (std::string const &name : sets) s_desc.sets.emplace_back(name, set_given(name));
		s_desc.files = files.size();
		out += ",\"sets\":[";
		for (size_t i = 0; i < sets.size(); i++)
			out += std::string(i ? "," : "") + "{\"name\":" + json(sets[i]) + ",\"given\":" + (set_given(sets[i]) ? "true" : "false") + "}";
		out += "],\"files\":" + std::to_string(files.size()) + ",\"problems\":[";
		bool first = true;
		for (needed_file const &f : files)
		{
			if (f.problem.empty()) continue;
			s_desc.problems.push_back(f);
			out += std::string(first ? "" : ",") + "{\"file\":" + json(f.name) + ",\"set\":" + json(f.set)
				+ ",\"disc\":" + (f.disc ? "true" : "false") + ",\"what\":" + json(f.problem) + "}";
			first = false;
		}
		out += "]";

		// the BIOS choices, for a machine that has more than one
		out += ",\"bios\":[";
		first = true;
		std::string default_bios;
		for (romload::system_bios const &bios : romload::entries(driver.rom).get_system_bioses())
		{
			out += std::string(first ? "" : ",") + "{\"name\":" + json(bios.get_name()) + ",\"description\":" + json(bios.get_description()) + "}";
			if (bios.get_value() == config.root_device().default_bios()) default_bios = bios.get_name();
			s_desc.bios.emplace_back(bios.get_name(), bios.get_description());
			first = false;
		}
		s_desc.default_bios = default_bios;
		out += "],\"defaultBios\":" + json(default_bios);

		// the switches
		ioport_list ports;
		std::ostringstream errors;
		for (device_t &device : device_enumerator(config.root_device()))
			ports.append(device, errors);
		out += ",\"switches\":[";
		first = true;
		std::set<std::string> seen;
		for (auto &port : ports)
		{
			for (ioport_field const &field : port.second->fields())
			{
				if (field.type() != IPT_DIPSWITCH && field.type() != IPT_CONFIG) continue;
				if (!field.specific_name() || !seen.insert(field.specific_name()).second) continue;
				std::string options_json, chosen;
				described_switch sw;
				for (ioport_setting const &setting : field.settings())
				{
					// two positions of one switch with one name are one option
					if (std::find(sw.options.begin(), sw.options.end(), setting.name()) != sw.options.end()) continue;
					sw.options.emplace_back(setting.name());
					options_json += (options_json.empty() ? "" : ",") + json(setting.name());
					if (setting.value() == field.defvalue()) chosen = setting.name();
				}
				if (options_json.empty()) continue;
				if (chosen.empty()) chosen = sw.options[0];
				sw.name = field.specific_name();
				sw.kind = field.type() == IPT_DIPSWITCH ? "dip" : "config";
				sw.chosen = chosen;
				s_desc.switches.push_back(std::move(sw));
				out += std::string(first ? "" : ",") + "{\"name\":" + json(field.specific_name())
					+ ",\"kind\":" + (field.type() == IPT_DIPSWITCH ? "\"dip\"" : "\"config\"")
					+ ",\"options\":[" + options_json + "],\"default\":" + json(chosen) + "}";
				first = false;
			}
		}
		out += "]}";
		s_description = out;
	}
	catch (std::exception const &e)
	{
		s_desc = described();
		s_desc.error = e.what();
		s_description = std::string("{\"error\":") + json(e.what()) + "}";
	}
	return s_description.c_str();
}

// The description in words: the game, what MAME says of it, the sets it reads
// from, and every file that is missing or wrong with the set it belongs in.
extern "C" const char *mame_report(void)
{
	static std::string out;
	if (!s_desc.error.empty()) return (out = s_desc.error).c_str();
	out = s_desc.title + ".";
	if (!s_desc.status.empty()) out += " MAME's notes: " + s_desc.status + ".";
	if (s_desc.files == 0)
		out += " This machine needs no files.";
	else
	{
		out += " It reads " + std::to_string(s_desc.files) + " files from";
		bool first = true;
		for (auto const &set : s_desc.sets)
		{
			// a set that was not given and is not missed: its files are in
			// another of the sets (a game's zip that carries its BIOS)
			bool missed = false;
			for (needed_file const &f : s_desc.problems) missed = missed || f.set == set.first;
			if (!set.second && !missed) continue;
			out += std::string(first ? "" : ",") + " " + set.first + ".zip" + (set.second ? "" : " (NOT GIVEN)");
			first = false;
		}
		out += ".";
	}
	if (s_desc.problems.empty())
	{
		if (s_desc.files) out += " All of them were found.";
	}
	else
	{
		out += " " + std::to_string(s_desc.problems.size()) + (s_desc.problems.size() == 1 ? " file is" : " files are") + " missing or wrong:";
		size_t shown = 0;
		for (needed_file const &f : s_desc.problems)
		{
			if (++shown > 12) { out += " ... and " + std::to_string(s_desc.problems.size() - 12) + " more"; break; }
			out += std::string(shown > 1 ? ";" : "") + " " + f.name + (f.disc ? ".chd (a disc image)" : " (in " + f.set + ".zip)") + " - " + f.problem;
		}
		out += ".";
	}
	return out.c_str();
}

extern "C" int mame_problem_count(void) { return s_desc.error.empty() ? int(s_desc.problems.size()) : 1; }

// The game's own settings, as Chimera setting declarations: its BIOS when it
// has more than one, then each switch. A setting's key is "bios",
// "dip.<name>" or "config.<name>".
extern "C" const char *mame_game_settings(void)
{
	static std::string out;
	out = "[";
	if (s_desc.bios.size() > 1)
	{
		std::string chosen = s_desc.bios[0].second;
		out += "{\"name\":\"bios\",\"display\":\"BIOS\",\"type\":\"enum\",\"options\":[";
		for (size_t i = 0; i < s_desc.bios.size(); i++)
		{
			out += (i ? "," : "") + json(s_desc.bios[i].second);
			if (s_desc.bios[i].first == s_desc.default_bios) chosen = s_desc.bios[i].second;
		}
		out += "],\"default\":" + json(chosen) + ",\"description\":"
			+ json("Which of the machine's BIOS versions it starts with. The default is MAME's. A movie needs the same one to play back.") + "}";
	}
	for (described_switch const &sw : s_desc.switches)
	{
		out += out.size() > 1 ? "," : "";
		out += "{\"name\":" + json(sw.kind + "." + sw.name) + ",\"display\":" + json(sw.name) + ",\"type\":\"enum\",\"options\":[";
		for (size_t i = 0; i < sw.options.size(); i++) out += (i ? "," : "") + json(sw.options[i]);
		out += "],\"default\":" + json(sw.chosen) + ",\"description\":"
			+ json(std::string(sw.kind == "dip" ? "A dip switch of this game's board" : "A configuration switch of this machine")
				+ ": '" + sw.name + "'. The game reads it as part of the machine, so a movie needs the same value to play back. The default is MAME's.") + "}";
	}
	return (out += "]").c_str();
}

extern "C" int mame_game_setting_count(void) { return int(s_desc.switches.size()) + (s_desc.bios.size() > 1 ? 1 : 0); }
extern "C" const char *mame_game_setting_key(int index)
{
	static std::string out;
	if (s_desc.bios.size() > 1 && index-- == 0) return "bios";
	if (index < 0 || index >= int(s_desc.switches.size())) return "";
	return (out = s_desc.switches[index].kind + "." + s_desc.switches[index].name).c_str();
}

// Gives one of those settings its value, before mame_init. 0 when the game
// has that setting and that value.
extern "C" int mame_apply_game_setting(const char *key, const char *value)
{
	if (!std::strcmp(key, "bios"))
	{
		for (auto const &bios : s_desc.bios)
			if (bios.second == value || bios.first == value) { s_options["bios"] = bios.first; return 0; }
		return 1;
	}
	for (described_switch const &sw : s_desc.switches)
	{
		if (sw.kind + "." + sw.name != key) continue;
		if (std::find(sw.options.begin(), sw.options.end(), value) == sw.options.end()) return 1;
		s_switches[sw.name] = value;
		return 0;
	}
	return 1;
}

extern "C" int mame_init(void)
{
	push_output();
	s_log.clear();
	try
	{
		s_emu_options = new emu_options();
		auto &o = *s_emu_options;
		fixed_options(o);
		o.set_system_name(s_game);
		for (auto const &kv : s_options)
			set(o, kv.first.c_str(), kv.second);
		if (!o.system())
			throw std::runtime_error("MAME has no machine named \"" + s_game + "\"");

		s_osd = new chimera_osd();
		s_manager = new chimera_manager(o, *s_osd);
		// what MAME's frontend always does; the machine asks for the server object
		// whether or not it serves anything, and with http off it serves nothing
		s_manager->start_http_server();
	}
	catch (std::exception const &e)
	{
		s_error = e.what();
		return 1;
	}
	s_host = co_active();
	s_mame = co_create(16 * 1024 * 1024, mame_main);
	co_switch(s_mame);
	if (!s_refused.empty())
	{
		s_error = s_refused;
		return 1;
	}
	return s_stopped ? 1 : 0;
}

extern "C" const char *mame_error(void) { return s_error.c_str(); }

extern "C" int mame_frame(void)
{
	if (s_stopped) return 1;
	s_audio.clear();
	s_input_read = false;
	apply_panel();
	co_switch(s_mame);
	return s_stopped ? 1 : 0;
}

extern "C" const uint32_t *mame_video(int *width, int *height)
{
	*width = s_width;
	*height = s_height;
	return s_video.data();
}

extern "C" void mame_aspect(int *x, int *y)
{
	*x = s_aspect_x;
	*y = s_aspect_y;
}

extern "C" const int16_t *mame_audio(int *frames)
{
	*frames = int(s_audio.size() / 2);
	return s_audio.data();
}

extern "C" int mame_sample_rate(void) { return kSampleRate; }

extern "C" int mame_refresh_millihertz(void)
{
	if (!s_machine) return 60000;
	screen_device *const screen = screen_device_enumerator(s_machine->root_device()).first();
	if (!screen) return 60000;
	return int(screen->frame_period().as_hz() * 1000.0 + 0.5);
}

extern "C" int mame_button_count(void) { return int(s_buttons.size()); }
extern "C" const char *mame_button_name(int i) { return i >= 0 && i < int(s_buttons.size()) ? s_buttons[i].name.c_str() : ""; }
extern "C" const char *mame_button_token(int i) { return i >= 0 && i < int(s_buttons.size()) ? s_buttons[i].token.c_str() : ""; }
extern "C" const char *mame_button_label(int i) { return i >= 0 && i < int(s_buttons.size()) ? s_buttons[i].label.c_str() : ""; }
extern "C" int mame_button_active(int i) { return i >= 0 && i < int(s_buttons.size()) && !s_buttons[i].fields.empty(); }
extern "C" void mame_set_button(int i, int pressed)
{
	if (i >= 0 && i < int(s_button_state.size())) s_button_state[i] = pressed ? 1 : 0;
}

extern "C" int mame_axis_count(void) { return int(s_axes.size()); }
extern "C" const char *mame_axis_name(int i) { return i >= 0 && i < int(s_axes.size()) ? s_axes[i].name.c_str() : ""; }
extern "C" const char *mame_axis_token(int i) { return i >= 0 && i < int(s_axes.size()) ? s_axes[i].token.c_str() : ""; }
extern "C" const char *mame_axis_label(int i) { return i >= 0 && i < int(s_axes.size()) ? s_axes[i].label.c_str() : ""; }
extern "C" int mame_axis_active(int i) { return i >= 0 && i < int(s_axes.size()) && !s_axes[i].fields.empty(); }
extern "C" void mame_axis_range(int i, int *min, int *max)
{
	*min = *max = 0;
	if (i < 0 || i >= int(s_axes.size()) || s_axes[i].fields.empty()) return;
	control const &c = s_axes[i];
	if (c.relative) { *min = -s32(c.bits); *max = s32(c.bits); }
	else { *min = c.min - c.rest; *max = c.max - c.rest; }
}
extern "C" void mame_set_axis(int i, int value)
{
	if (i >= 0 && i < int(s_axis_state.size())) s_axis_state[i] = value;
}

extern "C" int mame_input_was_read(void) { return s_input_read || s_controls_pushed ? 1 : 0; }

// ---- memory ----------------------------------------------------------------
namespace {
struct domain { std::string name; uint8_t *ptr; int64_t size; };
std::vector<domain> s_domains;
running_machine *s_domains_of;

void find_domains()
{
	if (s_domains_of == s_machine) return;
	s_domains.clear();
	s_domains_of = s_machine;
	if (!s_machine) return;
	// the machine's named memory, in name order (MAME keeps it unordered)
	// (":nvram" is nvram: MAME's tags start at the machine, which is ":")
	for (auto const &share : s_machine->memory().shares())
		s_domains.push_back({ share.first.c_str() + (share.first[0] == ':' ? 1 : 0), static_cast<uint8_t *>(share.second->ptr()), int64_t(share.second->bytes()) });
	std::sort(s_domains.begin(), s_domains.end(), [] (domain const &a, domain const &b) { return a.name < b.name; });
}
}

extern "C" int mame_domain_count(void) { find_domains(); return int(s_domains.size()); }
extern "C" const char *mame_domain_name(int i) { find_domains(); return i >= 0 && i < int(s_domains.size()) ? s_domains[i].name.c_str() : ""; }
extern "C" uint8_t *mame_domain_ptr(int i) { find_domains(); return i >= 0 && i < int(s_domains.size()) ? s_domains[i].ptr : nullptr; }
extern "C" int64_t mame_domain_size(int i) { find_domains(); return i >= 0 && i < int(s_domains.size()) ? s_domains[i].size : 0; }

// ---- save data --------------------------------------------------------------
extern "C" void mame_set_save_file(const char *name, const uint8_t *data, int64_t size)
{
	s_saves_in[name].assign(data, data + size);
}

extern "C" int mame_save_count(void)
{
	s_saves_out.clear();
	if (!s_machine) return 0;
	for (device_nvram_interface &nvram : nvram_interface_enumerator(s_machine->root_device()))
	{
		if (!nvram.nvram_can_save()) continue;
		save_file f;
		f.name = save_name(nvram.device());
		util::vector_read_write_adapter<uint8_t> file(f.bytes);
		if (nvram.nvram_save(file) && !f.bytes.empty())
			s_saves_out.push_back(std::move(f));
	}
	return int(s_saves_out.size());
}
extern "C" const char *mame_save_name(int i) { return i >= 0 && i < int(s_saves_out.size()) ? s_saves_out[i].name.c_str() : ""; }
extern "C" int64_t mame_save_size(int i) { return i >= 0 && i < int(s_saves_out.size()) ? int64_t(s_saves_out[i].bytes.size()) : 0; }
extern "C" const uint8_t *mame_save_data(int i) { return i >= 0 && i < int(s_saves_out.size()) ? s_saves_out[i].bytes.data() : nullptr; }

// ---- the address spaces ----------------------------------------------------
// Each processor's program space is a bus: memory at the addresses the game
// itself uses, in the processor's byte order. A read goes through MAME's own
// handlers with their side effects off, so reading a register changes
// nothing. The ranges are the map's RAM (what a RAM search is for); a space
// whose addresses are not bytes (a few signal processors') is left out.
namespace {
struct bus { std::string name; address_space *space; int64_t size; std::vector<int64_t> ranges; };
std::vector<bus> s_buses;
running_machine *s_buses_of;

void find_buses()
{
	if (s_buses_of == s_machine) return;
	s_buses.clear();
	s_buses_of = s_machine;
	if (!s_machine) return;
	for (device_memory_interface &memory : memory_interface_enumerator(s_machine->root_device()))
	{
		if (!memory.has_space(AS_PROGRAM)) continue;
		address_space &space = memory.space(AS_PROGRAM);
		if (space.addr_shift() != 0) continue;
		bus b;
		b.name = memory.device().tag() + 1;      // ":maincpu" is maincpu
		b.space = &space;
		b.size = int64_t(space.addrmask()) + 1;
		if (space.map())
			for (address_map_entry const &entry : space.map()->m_entrylist)
				if (entry.m_read.m_type == AMH_RAM && entry.m_write.m_type != AMH_NONE && entry.m_addrend >= entry.m_addrstart)
				{
					b.ranges.push_back(entry.m_addrstart);
					b.ranges.push_back(int64_t(entry.m_addrend) - entry.m_addrstart + 1);
				}
		b.ranges.push_back(0);
		b.ranges.push_back(0);
		// the main processor first, the others in MAME's order
		if (b.name == "maincpu") s_buses.insert(s_buses.begin(), std::move(b));
		else s_buses.push_back(std::move(b));
	}
}
}

extern "C" int mame_bus_count(void) { find_buses(); return int(s_buses.size()); }
extern "C" const char *mame_bus_name(int i) { find_buses(); return i >= 0 && i < int(s_buses.size()) ? s_buses[i].name.c_str() : ""; }
extern "C" int64_t mame_bus_size(int i) { find_buses(); return i >= 0 && i < int(s_buses.size()) ? s_buses[i].size : 0; }
extern "C" const int64_t *mame_bus_ranges(int i) { find_buses(); return i >= 0 && i < int(s_buses.size()) ? s_buses[i].ranges.data() : nullptr; }
extern "C" void mame_bus_read(int i, int64_t address, uint8_t *out, int length)
{
	find_buses();
	std::memset(out, 0, length);
	if (i < 0 || i >= int(s_buses.size())) return;
	auto const quiet = s_machine->disable_side_effects();
	address_space &space = *s_buses[i].space;
	for (int n = 0; n < length; n++)
		out[n] = space.read_byte(offs_t(address + n) & space.addrmask());
}
extern "C" void mame_bus_write(int i, int64_t address, int value)
{
	find_buses();
	if (i < 0 || i >= int(s_buses.size())) return;
	address_space &space = *s_buses[i].space;
	space.write_byte(offs_t(address) & space.addrmask(), u8(value));
}

extern "C" const char *mame_list_switches(void)
{
	static std::string out;
	out.clear();
	if (!s_machine) return "";
	for (auto &port : s_machine->ioport().ports())
	{
		for (ioport_field &field : port.second->fields())
		{
			if (field.type() != IPT_DIPSWITCH && field.type() != IPT_CONFIG) continue;
			if (!field.specific_name()) continue;
			ioport_field::user_settings us;
			field.get_user_settings(us);
			std::string now, all;
			for (ioport_setting const &setting : field.settings())
			{
				if (setting.value() == us.value) now = setting.name();
				all += (all.empty() ? "" : " | ") + std::string(setting.name());
			}
			out += std::string(field.specific_name()) + "\t" + now + "\t(" + all + ")\n";
		}
	}
	return out.c_str();
}

// ---- the whole database ----------------------------------------------------
// Walks every machine of this build without running any: builds its
// description as MAME does for its own listings, and reports what the panel
// has to hold - which controls are used at all, how many controls with no
// type of their own one machine can have, and anything the panel cannot
// carry. A machine must be running (any: Pong needs no files), because the
// list of control types is the running machine's. One line a finding:
//   used <token>            a control at least one machine has
//   nofiles <machine>       a machine that needs no files at all
//   unplaced <machine> ...  a control the panel has no place for
//   summary ...
extern "C" void mame_scan(void)
{
	if (!s_machine) { std::puts("summary no machine is running"); return; }
	// every control MAME knows, whatever the panel was built with
	std::vector<control> all_buttons, all_axes;
	all_controls(*s_machine, all_buttons, all_axes);
	int const first_other = int(all_buttons.size());
	std::map<std::pair<int, int>, int> button_at, axis_at;
	for (int i = 0; i < first_other; i++) button_at[{ int(all_buttons[i].type), all_buttons[i].player }] = i;
	for (int i = 0; i < int(all_axes.size()); i++) axis_at[{ int(all_axes[i].type), all_axes[i].player }] = i;
	std::vector<int> button_used(all_buttons.size()), axis_used(all_axes.size());
	// the widest an axis is asked to go over all machines, from its rest
	std::vector<s32> axis_lo(all_axes.size()), axis_hi(all_axes.size());
	size_t machines = 0, failed = 0, unplaced = 0, most_other = 0, with_axes = 0, odd_range = 0, not_working = 0, mechanical = 0;
	std::string most_other_name;
	for (size_t at = 0; at < driver_list::total(); at++)
	{
		game_driver const &driver = driver_list::driver(at);
		if (!std::strcmp(driver.name, "___empty")) continue;
		try
		{
			emu_options options;
			fixed_options(options);
			options.set_system_name(driver.name);
			machine_config config(driver, options);
			ioport_list ports;
			std::ostringstream errors;
			bool files = false;
			for (device_t &device : device_enumerator(config.root_device()))
			{
				ports.append(device, errors);
				for (rom_entry const *region = rom_first_region(device); region && !files; region = rom_next_region(region))
					files = rom_first_file(region) != nullptr;
			}
			machines++;
			if (driver.type.emulation_flags() & device_t::flags::NOT_WORKING) not_working++;
			if (driver.flags & MACHINE_MECHANICAL) mechanical++;
			if (!files) std::printf("nofiles %s\n", driver.name);
			size_t other = 0;
			bool axes = false;
			for (auto &port : ports)
			{
				for (ioport_field const &field : port.second->fields())
				{
					// a control, as ioport_field::type_class tells them apart - asked
					// of the running machine, because these fields belong to none
					if (field.type() == IPT_UNUSED || field.type() == IPT_UNKNOWN || field.type() == IPT_SPECIAL
						|| field.type() == IPT_CUSTOM || field.type() == IPT_OUTPUT || field.type() == IPT_DIPSWITCH
						|| field.type() == IPT_CONFIG || field.type() == IPT_ADJUSTER) continue;
					ioport_group const group = s_machine->ioport().type_group(field.type(), field.player());
					if (group == IPG_UI) continue;
					if (group == IPG_INVALID && !(is_nameless(field.type()) && field.specific_name())) continue;
					if (field.is_analog())
					{
						auto const found = axis_at.find({ int(field.type()), int(field.player()) });
						if (found == axis_at.end()) { unplaced++; std::printf("unplaced %s axis type %d player %d\n", driver.name, int(field.type()), int(field.player())); continue; }
						axis_used[found->second]++;
						axes = true;
						if (field.minval() > field.maxval()) odd_range++;
						analog_shape const a = shape_of(field);
						bool const relative = a.relative;
						s32 const lo = a.min, hi = a.max, rest = a.rest;
						axis_lo[found->second] = std::min(axis_lo[found->second], relative ? -(hi - lo) : lo - rest);
						axis_hi[found->second] = std::max(axis_hi[found->second], relative ? hi - lo : hi - rest);
					}
					else if (is_nameless(field.type()))
						other++;
					else
					{
						auto const found = button_at.find({ int(field.type()), int(field.player()) });
						if (found == button_at.end()) { unplaced++; std::printf("unplaced %s button type %d player %d\n", driver.name, int(field.type()), int(field.player())); continue; }
						button_used[found->second]++;
					}
				}
			}
			if (axes) with_axes++;
			if (other > most_other) { most_other = other; most_other_name = driver.name; }
		}
		catch (std::exception const &e)
		{
			failed++;
			std::printf("failed %s %s\n", driver.name, e.what());
		}
	}
	size_t buttons = 0, axes = 0;
	for (int i = 0; i < first_other; i++)
		if (button_used[i]) { buttons++; std::printf("used %s\t%s\t%d\tP%d\n", all_buttons[i].token.c_str(), all_buttons[i].name.c_str(), button_used[i], all_buttons[i].player + 1); }
	for (size_t i = 0; i < all_axes.size(); i++)
		if (axis_used[i]) { axes++; std::printf("usedaxis %s\t%s\t%d\tP%d\t%d\t%d\n", all_axes[i].token.c_str(), all_axes[i].name.c_str(), axis_used[i], all_axes[i].player + 1, axis_lo[i], axis_hi[i]); }
	std::printf("summary machines %zu failed %zu notworking %zu mechanical %zu buttons %zu of %d axes %zu of %zu unplaced %zu mostother %zu (%s) withaxes %zu minovermax %zu\n",
		machines, failed, not_working, mechanical, buttons, first_other, axes, all_axes.size(), unplaced, most_other, most_other_name.c_str(), with_axes, odd_range);
}
