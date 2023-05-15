/*
 * libdivecomputer
 *
 * Copyright (C) 2016 Jef Driesen
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301 USA
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <libdivecomputer/units.h>

#include "output-private.h"
#include "utils.h"

static dc_status_t dctool_xml_output_write (dctool_output_t *output, dc_parser_t *parser, const unsigned char data[], unsigned int size, const unsigned char fingerprint[], unsigned int fsize);
static dc_status_t dctool_xml_output_free (dctool_output_t *output);

typedef struct dctool_xml_output_t {
	dctool_output_t base;
	FILE *ostream;
	dctool_units_t units;
} dctool_xml_output_t;

static const dctool_output_vtable_t xml_vtable = {
	sizeof(dctool_xml_output_t), /* size */
	dctool_xml_output_write, /* write */
	dctool_xml_output_free, /* free */
};

typedef struct sample_data_t {
	FILE *ostream;
	dctool_units_t units;
	unsigned int nsamples;
} sample_data_t;

static double
convert_depth (double value, dctool_units_t units)
{
	if (units == DCTOOL_UNITS_IMPERIAL) {
		return value / FEET;
	} else {
		return value;
	}
}

static double
convert_temperature (double value, dctool_units_t units)
{
	if (units == DCTOOL_UNITS_IMPERIAL) {
		return value * (9.0 / 5.0) + 32.0;
	} else {
		return value;
	}
}

static double
convert_pressure (double value, dctool_units_t units)
{
	if (units == DCTOOL_UNITS_IMPERIAL) {
		return value * BAR / PSI;
	} else {
		return value;
	}
}

static double
convert_volume (double value, dctool_units_t units)
{
	if (units == DCTOOL_UNITS_IMPERIAL) {
		return value / 1000.0 / CUFT;
	} else {
		return value;
	}
}

static void
sample_cb (dc_sample_type_t type, const dc_sample_value_t *value, void *userdata)
{
	static const char *events[] = {
		"none", "deco", "rbt", "ascent", "ceiling", "workload", "transmitter",
		"violation", "bookmark", "surface", "safety stop", "gaschange",
		"safety stop (voluntary)", "safety stop (mandatory)", "deepstop",
		"ceiling (safety stop)", "floor", "divetime", "maxdepth",
		"OLF", "PO2", "airtime", "rgbm", "heading", "tissue level warning",
		"gaschange2"};
	static const char *decostop[] = {
		"ndl", "safety", "deco", "deep"};

	sample_data_t *sampledata = (sample_data_t *) userdata;

	unsigned int seconds = 0, milliseconds = 0;

	switch (type) {
	case DC_SAMPLE_TIME:
		seconds = value->time / 1000;
		milliseconds = value->time % 1000;
		if (sampledata->nsamples++)
			fprintf (sampledata->ostream, "</sample>\n");
		fprintf (sampledata->ostream, "<sample>\n");
		if (milliseconds) {
			fprintf (sampledata->ostream, "   <time>%02u:%02u.%03u</time>\n", seconds / 60, seconds % 60, milliseconds);
		} else {
			fprintf (sampledata->ostream, "   <time>%02u:%02u</time>\n", seconds / 60, seconds % 60);
		}
		break;
	case DC_SAMPLE_DEPTH:
		fprintf (sampledata->ostream, "   <depth>%.2f</depth>\n",
			convert_depth(value->depth, sampledata->units));
		break;
	case DC_SAMPLE_PRESSURE:
		fprintf (sampledata->ostream, "   <pressure tank=\"%u\">%.2f</pressure>\n",
			value->pressure.tank,
			convert_pressure(value->pressure.value, sampledata->units));
		break;
	case DC_SAMPLE_TEMPERATURE:
		fprintf (sampledata->ostream, "   <temperature>%.2f</temperature>\n",
			convert_temperature(value->temperature, sampledata->units));
		break;
	case DC_SAMPLE_EVENT:
		if (value->event.type != SAMPLE_EVENT_GASCHANGE && value->event.type != SAMPLE_EVENT_GASCHANGE2) {
			fprintf (sampledata->ostream, "   <event type=\"%u\" time=\"%u\" flags=\"%u\" value=\"%u\">%s</event>\n",
				value->event.type, value->event.time, value->event.flags, value->event.value, events[value->event.type]);
		}
		break;
	case DC_SAMPLE_RBT:
		fprintf (sampledata->ostream, "   <rbt>%u</rbt>\n", value->rbt);
		break;
	case DC_SAMPLE_HEARTBEAT:
		fprintf (sampledata->ostream, "   <heartbeat>%u</heartbeat>\n", value->heartbeat);
		break;
	case DC_SAMPLE_BEARING:
		fprintf (sampledata->ostream, "   <bearing>%u</bearing>\n", value->bearing);
		break;
	case DC_SAMPLE_VENDOR:
		fprintf (sampledata->ostream, "   <vendor type=\"%u\" size=\"%u\">", value->vendor.type, value->vendor.size);
		for (unsigned int i = 0; i < value->vendor.size; ++i)
			fprintf (sampledata->ostream, "%02X", ((const unsigned char *) value->vendor.data)[i]);
		fprintf (sampledata->ostream, "</vendor>\n");
		break;
	case DC_SAMPLE_SETPOINT:
		fprintf (sampledata->ostream, "   <setpoint>%.2f</setpoint>\n", value->setpoint);
		break;
	case DC_SAMPLE_PPO2:
		if (value->ppo2.sensor != DC_SENSOR_NONE) {
			fprintf (sampledata->ostream, "   <ppo2 sensor=\"%u\">%.2f</ppo2>\n", value->ppo2.sensor, value->ppo2.value);
		} else {
			fprintf (sampledata->ostream, "   <ppo2>%.2f</ppo2>\n", value->ppo2.value);
		}
		break;
	case DC_SAMPLE_CNS:
		fprintf (sampledata->ostream, "   <cns>%.1f</cns>\n", value->cns * 100.0);
		break;
	case DC_SAMPLE_DECO:
		fprintf (sampledata->ostream, "   <deco time=\"%u\" depth=\"%.2f\">%s</deco>\n",
			value->deco.time,
			convert_depth(value->deco.depth, sampledata->units),
			decostop[value->deco.type]);
		if (value->deco.tts) {
			fprintf (sampledata->ostream, "   <tts>%u</tts>\n",
				value->deco.tts);
		}
		break;
	case DC_SAMPLE_GASMIX:
		fprintf (sampledata->ostream, "   <gasmix>%u</gasmix>\n", value->gasmix);
		break;
	default:
		break;
	}
}

dctool_output_t *
dctool_xml_output_new (const char *filename, dctool_units_t units)
{
	dctool_xml_output_t *output = NULL;

	if (filename == NULL)
		goto error_exit;

	// Allocate memory.
	output = (dctool_xml_output_t *) dctool_output_allocate (&xml_vtable);
	if (output == NULL) {
		goto error_exit;
	}

	// Open the output file.
	output->ostream = fopen (filename, "w");
	if (output->ostream == NULL) {
		goto error_free;
	}

	output->units = units;

	fprintf (output->ostream, "<device>\n");

	return (dctool_output_t *) output;

error_free:
	dctool_output_deallocate ((dctool_output_t *) output);
error_exit:
	return NULL;
}

static dc_status_t
dctool_xml_output_write (dctool_output_t *abstract, dc_parser_t *parser, const unsigned char data[], unsigned int size, const unsigned char fingerprint[], unsigned int fsize)
{
	dctool_xml_output_t *output = (dctool_xml_output_t *) abstract;
	dc_status_t status = DC_STATUS_SUCCESS;

	// Initialize the sample data.
	sample_data_t sampledata = {0};
	sampledata.nsamples = 0;
	sampledata.ostream = output->ostream;
	sampledata.units = output->units;

	fprintf (output->ostream, "<dive>\n<number>%u</number>\n<size>%u</size>\n", abstract->number, size);

	if (fingerprint) {
		fprintf (output->ostream, "<fingerprint>");
		for (unsigned int i = 0; i < fsize; ++i)
			fprintf (output->ostream, "%02X", fingerprint[i]);
		fprintf (output->ostream, "</fingerprint>\n");
	}

	// Parse the datetime.
	message ("Parsing the datetime.\n");
	dc_datetime_t dt = {0};
	status = dc_parser_get_datetime (parser, &dt);
	if (status != DC_STATUS_SUCCESS && status != DC_STATUS_UNSUPPORTED) {
		ERROR ("Error parsing the datetime.");
		goto cleanup;
	}

	if (dt.timezone == DC_TIMEZONE_NONE) {
		fprintf (output->ostream, "<datetime>%04i-%02i-%02i %02i:%02i:%02i</datetime>\n",
			dt.year, dt.month, dt.day,
			dt.hour, dt.minute, dt.second);
	} else {
		fprintf (output->ostream, "<datetime>%04i-%02i-%02i %02i:%02i:%02i %+03i:%02i</datetime>\n",
			dt.year, dt.month, dt.day,
			dt.hour, dt.minute, dt.second,
			dt.timezone / 3600, (abs(dt.timezone) % 3600) / 60);
	}

	// Parse the divetime.
	message ("Parsing the divetime.\n");
	unsigned int divetime = 0;
	status = dc_parser_get_field (parser, DC_FIELD_DIVETIME, 0, &divetime);
	if (status != DC_STATUS_SUCCESS && status != DC_STATUS_UNSUPPORTED) {
		ERROR ("Error parsing the divetime.");
		goto cleanup;
	}

	fprintf (output->ostream, "<divetime>%02u:%02u</divetime>\n",
		divetime / 60, divetime % 60);

	// Parse the maxdepth.
	message ("Parsing the maxdepth.\n");
	double maxdepth = 0.0;
	status = dc_parser_get_field (parser, DC_FIELD_MAXDEPTH, 0, &maxdepth);
	if (status != DC_STATUS_SUCCESS && status != DC_STATUS_UNSUPPORTED) {
		ERROR ("Error parsing the maxdepth.");
		goto cleanup;
	}

	fprintf (output->ostream, "<maxdepth>%.2f</maxdepth>\n",
		convert_depth(maxdepth, output->units));

	// Parse the avgdepth.
	message ("Parsing the avgdepth.\n");
	double avgdepth = 0.0;
	status = dc_parser_get_field (parser, DC_FIELD_AVGDEPTH, 0, &avgdepth);
	if (status != DC_STATUS_SUCCESS && status != DC_STATUS_UNSUPPORTED) {
		ERROR ("Error parsing the avgdepth.");
		goto cleanup;
	}

	if (status != DC_STATUS_UNSUPPORTED) {
		fprintf (output->ostream, "<avgdepth>%.2f</avgdepth>\n",
			convert_depth(avgdepth, output->units));
	}

	// Parse the temperature.
	message ("Parsing the temperature.\n");
	for (unsigned int i = 0; i < 3; ++i) {
		dc_field_type_t fields[] = {DC_FIELD_TEMPERATURE_SURFACE,
			DC_FIELD_TEMPERATURE_MINIMUM,
			DC_FIELD_TEMPERATURE_MAXIMUM};
		const char *names[] = {"surface", "minimum", "maximum"};

		double temperature = 0.0;
		status = dc_parser_get_field (parser, fields[i], 0, &temperature);
		if (status != DC_STATUS_SUCCESS && status != DC_STATUS_UNSUPPORTED) {
			ERROR ("Error parsing the temperature.");
			goto cleanup;
		}

		if (status != DC_STATUS_UNSUPPORTED) {
			fprintf (output->ostream, "<temperature type=\"%s\">%.1f</temperature>\n",
				names[i],
				convert_temperature(temperature, output->units));
		}
	}

	// Parse the gas mixes.
	message ("Parsing the gas mixes.\n");
	unsigned int ngases = 0;
	status = dc_parser_get_field (parser, DC_FIELD_GASMIX_COUNT, 0, &ngases);
	if (status != DC_STATUS_SUCCESS && status != DC_STATUS_UNSUPPORTED) {
		ERROR ("Error parsing the gas mix count.");
		goto cleanup;
	}

	for (unsigned int i = 0; i < ngases; ++i) {
		dc_gasmix_t gasmix = {0};
		status = dc_parser_get_field (parser, DC_FIELD_GASMIX, i, &gasmix);
		if (status != DC_STATUS_SUCCESS && status != DC_STATUS_UNSUPPORTED) {
			ERROR ("Error parsing the gas mix.");
			goto cleanup;
		}

		fprintf (output->ostream,
			"<gasmix>\n"
			"   <he>%.1f</he>\n"
			"   <o2>%.1f</o2>\n"
			"   <n2>%.1f</n2>\n",
			gasmix.helium * 100.0,
			gasmix.oxygen * 100.0,
			gasmix.nitrogen * 100.0);
		if (gasmix.usage) {
			const char *usage[] = {"none", "oxygen", "diluent", "sidemount"};
			fprintf (output->ostream,
				"   <usage>%s</usage>\n",
				usage[gasmix.usage]);
		}
		fprintf (output->ostream,
			"</gasmix>\n");

	}

	// Parse the tanks.
	message ("Parsing the tanks.\n");
	unsigned int ntanks = 0;
	status = dc_parser_get_field (parser, DC_FIELD_TANK_COUNT, 0, &ntanks);
	if (status != DC_STATUS_SUCCESS && status != DC_STATUS_UNSUPPORTED) {
		ERROR ("Error parsing the tank count.");
		goto cleanup;
	}

	for (unsigned int i = 0; i < ntanks; ++i) {
		const char *names[] = {"none", "metric", "imperial"};

		dc_tank_t tank = {0};
		status = dc_parser_get_field (parser, DC_FIELD_TANK, i, &tank);
		if (status != DC_STATUS_SUCCESS && status != DC_STATUS_UNSUPPORTED) {
			ERROR ("Error parsing the tank.");
			goto cleanup;
		}

		fprintf (output->ostream, "<tank>\n");
		if (tank.gasmix != DC_GASMIX_UNKNOWN) {
			fprintf (output->ostream,
				"   <gasmix>%u</gasmix>\n",
				tank.gasmix);
		}
		if (tank.usage) {
			const char *usage[] = {"none", "oxygen", "diluent", "sidemount"};
			fprintf (output->ostream,
				"   <usage>%s</usage>\n",
				usage[tank.usage]);
		}
		if (tank.type != DC_TANKVOLUME_NONE) {
			fprintf (output->ostream,
				"   <type>%s</type>\n"
				"   <volume>%.1f</volume>\n"
				"   <workpressure>%.2f</workpressure>\n",
				names[tank.type],
				convert_volume(tank.volume, output->units),
				convert_pressure(tank.workpressure, output->units));
		}
		fprintf (output->ostream,
			"   <beginpressure>%.2f</beginpressure>\n"
			"   <endpressure>%.2f</endpressure>\n"
			"</tank>\n",
			convert_pressure(tank.beginpressure, output->units),
			convert_pressure(tank.endpressure, output->units));
	}

	// Parse the dive mode.
	message ("Parsing the dive mode.\n");
	dc_divemode_t divemode = DC_DIVEMODE_OC;
	status = dc_parser_get_field (parser, DC_FIELD_DIVEMODE, 0, &divemode);
	if (status != DC_STATUS_SUCCESS && status != DC_STATUS_UNSUPPORTED) {
		ERROR ("Error parsing the dive mode.");
		goto cleanup;
	}

	if (status != DC_STATUS_UNSUPPORTED) {
		const char *names[] = {"freedive", "gauge", "oc", "ccr", "scr"};
		fprintf (output->ostream, "<divemode>%s</divemode>\n",
			names[divemode]);
	}

	// Parse the deco model.
	message ("Parsing the deco model.\n");
	dc_decomodel_t decomodel = {DC_DECOMODEL_NONE};
	status = dc_parser_get_field (parser, DC_FIELD_DECOMODEL, 0, &decomodel);
	if (status != DC_STATUS_SUCCESS && status != DC_STATUS_UNSUPPORTED) {
		ERROR ("Error parsing the deco model.");
		goto cleanup;
	}

	if (status != DC_STATUS_UNSUPPORTED) {
		const char *names[] = {"none", "buhlmann", "vpm", "rgbm", "dciem"};
		fprintf (output->ostream, "<decomodel>%s</decomodel>\n",
			names[decomodel.type]);
		if (decomodel.type == DC_DECOMODEL_BUHLMANN &&
			(decomodel.params.gf.low != 0 || decomodel.params.gf.high != 0)) {
			fprintf (output->ostream, "<gf>%u/%u</gf>\n",
				decomodel.params.gf.low, decomodel.params.gf.high);
		}
		if (decomodel.conservatism) {
			fprintf (output->ostream, "<conservatism>%d</conservatism>\n",
				decomodel.conservatism);
		}
	}

	// Parse the salinity.
	message ("Parsing the salinity.\n");
	dc_salinity_t salinity = {DC_WATER_FRESH, 0.0};
	status = dc_parser_get_field (parser, DC_FIELD_SALINITY, 0, &salinity);
	if (status != DC_STATUS_SUCCESS && status != DC_STATUS_UNSUPPORTED) {
		ERROR ("Error parsing the salinity.");
		goto cleanup;
	}

	if (status != DC_STATUS_UNSUPPORTED) {
		const char *names[] = {"fresh", "salt"};
		if (salinity.density) {
			fprintf (output->ostream, "<salinity density=\"%.1f\">%s</salinity>\n",
				salinity.density, names[salinity.type]);
		} else {
			fprintf (output->ostream, "<salinity>%s</salinity>\n",
				names[salinity.type]);
		}
	}

	// Parse the atmospheric pressure.
	message ("Parsing the atmospheric pressure.\n");
	double atmospheric = 0.0;
	status = dc_parser_get_field (parser, DC_FIELD_ATMOSPHERIC, 0, &atmospheric);
	if (status != DC_STATUS_SUCCESS && status != DC_STATUS_UNSUPPORTED) {
		ERROR ("Error parsing the atmospheric pressure.");
		goto cleanup;
	}

	if (status != DC_STATUS_UNSUPPORTED) {
		fprintf (output->ostream, "<atmospheric>%.5f</atmospheric>\n",
			convert_pressure(atmospheric, output->units));
	}

	// Parse the sample data.
	message ("Parsing the sample data.\n");
	status = dc_parser_samples_foreach (parser, sample_cb, &sampledata);
	if (status != DC_STATUS_SUCCESS) {
		ERROR ("Error parsing the sample data.");
		goto cleanup;
	}

cleanup:

	if (sampledata.nsamples)
		fprintf (output->ostream, "</sample>\n");
	fprintf (output->ostream, "</dive>\n");

	return status;
}

static dc_status_t
dctool_xml_output_free (dctool_output_t *abstract)
{
	dctool_xml_output_t *output = (dctool_xml_output_t *) abstract;

	fprintf (output->ostream, "</device>\n");

	fclose (output->ostream);

	return DC_STATUS_SUCCESS;
}
