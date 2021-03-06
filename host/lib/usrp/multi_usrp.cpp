//
// Copyright 2010-2011 Ettus Research LLC
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include <uhd/usrp/multi_usrp.hpp>
#include <uhd/usrp/tune_helper.hpp>
#include <uhd/usrp/mboard_iface.hpp>
#include <uhd/utils/msg.hpp>
#include <uhd/exception.hpp>
#include <uhd/utils/msg.hpp>
#include <uhd/utils/gain_group.hpp>
#include <uhd/usrp/subdev_props.hpp>
#include <uhd/usrp/mboard_props.hpp>
#include <uhd/usrp/device_props.hpp>
#include <uhd/usrp/dboard_props.hpp>
#include <uhd/usrp/dsp_props.hpp>
#include <boost/thread.hpp>
#include <boost/foreach.hpp>
#include <boost/format.hpp>
#include <cmath>

using namespace uhd;
using namespace uhd::usrp;

const std::string multi_usrp::ALL_GAINS = "";

/***********************************************************************
 * Helper methods
 **********************************************************************/
static inline uhd::freq_range_t add_dsp_shift(
    const uhd::freq_range_t &range,
    wax::obj dsp
){
    double codec_rate = dsp[uhd::usrp::DSP_PROP_CODEC_RATE].as<double>();
    return uhd::freq_range_t(range.start() - codec_rate/2.0, range.stop() + codec_rate/2.0);
}

static inline void do_samp_rate_warning_message(
    double target_rate,
    double actual_rate,
    const std::string &xx
){
    static const double max_allowed_error = 1.0; //Sps
    if (std::abs(target_rate - actual_rate) > max_allowed_error){
        UHD_MSG(warning) << boost::format(
            "The hardware does not support the requested %s sample rate:\n"
            "Target sample rate: %f MSps\n"
            "Actual sample rate: %f MSps\n"
        ) % xx % (target_rate/1e6) % (actual_rate/1e6);
    }
}

static inline void do_tune_freq_warning_message(
    double target_freq,
    double actual_freq,
    const std::string &xx
){
    static const double max_allowed_error = 1.0; //Hz
    if (std::abs(target_freq - actual_freq) > max_allowed_error){
        UHD_MSG(warning) << boost::format(
            "The hardware does not support the requested %s frequency:\n"
            "Target frequency: %f MHz\n"
            "Actual frequency: %f MHz\n"
        ) % xx % (target_freq/1e6) % (actual_freq/1e6);
    }
}

/***********************************************************************
 * Multi USRP Implementation
 **********************************************************************/
class multi_usrp_impl : public multi_usrp{
public:
    multi_usrp_impl(const device_addr_t &addr){
        _dev = device::make(addr);
    }

    device::sptr get_device(void){
        return _dev;
    }

    /*******************************************************************
     * Mboard methods
     ******************************************************************/
    void set_master_clock_rate(double rate, size_t mboard){
        if (mboard != ALL_MBOARDS){
            _mboard(mboard)[MBOARD_PROP_CLOCK_RATE] = rate;
            return;
        }
        for (size_t m = 0; m < get_num_mboards(); m++){
            set_master_clock_rate(rate, m);
        }
    }

    double get_master_clock_rate(size_t mboard){
        return _mboard(mboard)[MBOARD_PROP_CLOCK_RATE].as<double>();
    }

    std::string get_pp_string(void){
        std::string buff = str(boost::format(
            "%s USRP:\n"
            "  Device: %s\n"
        )
            % ((get_num_mboards() > 1)? "Multi" : "Single")
            % (*_dev)[DEVICE_PROP_NAME].as<std::string>()
        );
        for (size_t m = 0; m < get_num_mboards(); m++){
            buff += str(boost::format(
                "  Mboard %d: %s\n"
            ) % m
                % _mboard(m)[MBOARD_PROP_NAME].as<std::string>()
            );
        }

        //----------- rx side of life ----------------------------------
        for (size_t m = 0, chan = 0; m < get_num_mboards(); m++){
            for (; chan < (m + 1)*get_rx_subdev_spec(m).size(); chan++){
                buff += str(boost::format(
                    "  RX Channel: %u\n"
                    "    RX DSP: %s\n"
                    "    RX Dboard: %s\n"
                    "    RX Subdev: %s\n"
                ) % chan
                    % _rx_dsp(chan)[DSP_PROP_NAME].as<std::string>()
                    % _rx_dboard(chan)[DBOARD_PROP_NAME].as<std::string>()
                    % _rx_subdev(chan)[SUBDEV_PROP_NAME].as<std::string>()
                );
            }
        }

        //----------- tx side of life ----------------------------------
        for (size_t m = 0, chan = 0; m < get_num_mboards(); m++){
            for (; chan < (m + 1)*get_tx_subdev_spec(m).size(); chan++){
                buff += str(boost::format(
                    "  TX Channel: %u\n"
                    "    TX DSP: %s\n"
                    "    TX Dboard: %s\n"
                    "    TX Subdev: %s\n"
                ) % chan
                    % _tx_dsp(chan)[DSP_PROP_NAME].as<std::string>()
                    % _tx_dboard(chan)[DBOARD_PROP_NAME].as<std::string>()
                    % _tx_subdev(chan)[SUBDEV_PROP_NAME].as<std::string>()
                );
            }
        }

        return buff;
    }

    std::string get_mboard_name(size_t mboard){
        return _mboard(mboard)[MBOARD_PROP_NAME].as<std::string>();
    }

    time_spec_t get_time_now(size_t mboard = 0){
        return _mboard(mboard)[MBOARD_PROP_TIME_NOW].as<time_spec_t>();
    }

    time_spec_t get_time_last_pps(size_t mboard = 0){
        return _mboard(mboard)[MBOARD_PROP_TIME_PPS].as<time_spec_t>();
    }

    void set_time_now(const time_spec_t &time_spec, size_t mboard){
        if (mboard != ALL_MBOARDS){
            _mboard(mboard)[MBOARD_PROP_TIME_NOW] = time_spec;
            return;
        }
        for (size_t m = 0; m < get_num_mboards(); m++){
            set_time_now(time_spec, m);
        }
    }

    void set_time_next_pps(const time_spec_t &time_spec){
        for (size_t m = 0; m < get_num_mboards(); m++){
            _mboard(m)[MBOARD_PROP_TIME_PPS] = time_spec;
        }
    }

    void set_time_unknown_pps(const time_spec_t &time_spec){
        UHD_MSG(status) << "    1) catch time transition at pps edge" << std::endl;
        time_spec_t time_start = get_time_now();
        time_spec_t time_start_last_pps = get_time_last_pps();
        while(true){
            if (get_time_last_pps() != time_start_last_pps) break;
            if ((get_time_now() - time_start) > time_spec_t(1.1)){
                throw uhd::runtime_error(
                    "Board 0 may not be getting a PPS signal!\n"
                    "No PPS detected within the time interval.\n"
                    "See the application notes for your device.\n"
                );
            }
        }

        UHD_MSG(status) << "    2) set times next pps (synchronously)" << std::endl;
        set_time_next_pps(time_spec);
        boost::this_thread::sleep(boost::posix_time::seconds(1));

        //verify that the time registers are read to be within a few RTT
        for (size_t m = 1; m < get_num_mboards(); m++){
            time_spec_t time_0 = _mboard(0)[MBOARD_PROP_TIME_NOW].as<time_spec_t>();
            time_spec_t time_i = _mboard(m)[MBOARD_PROP_TIME_NOW].as<time_spec_t>();
            if (time_i < time_0 or (time_i - time_0) > time_spec_t(0.01)){ //10 ms: greater than RTT but not too big
                UHD_MSG(warning) << boost::format(
                    "Detected time deviation between board %d and board 0.\n"
                    "Board 0 time is %f seconds.\n"
                    "Board %d time is %f seconds.\n"
                ) % m % time_0.get_real_secs() % m % time_i.get_real_secs();
            }
        }
    }

    bool get_time_synchronized(void){
        for (size_t m = 1; m < get_num_mboards(); m++){
            time_spec_t time_0 = _mboard(0)[MBOARD_PROP_TIME_NOW].as<time_spec_t>();
            time_spec_t time_i = _mboard(m)[MBOARD_PROP_TIME_NOW].as<time_spec_t>();
            if (time_i < time_0 or (time_i - time_0) > time_spec_t(0.01)) return false;
        }
        return true;
    }

    void issue_stream_cmd(const stream_cmd_t &stream_cmd, size_t chan){
        if (chan != ALL_CHANS){
            _rx_dsp(chan)[DSP_PROP_STREAM_CMD] = stream_cmd;
            return;
        }
        for (size_t c = 0; c < get_rx_num_channels(); c++){
            issue_stream_cmd(stream_cmd, c);
        }
    }

    void set_clock_config(const clock_config_t &clock_config, size_t mboard){
        if (mboard != ALL_MBOARDS){
            _mboard(mboard)[MBOARD_PROP_CLOCK_CONFIG] = clock_config;
            return;
        }
        for (size_t m = 0; m < get_num_mboards(); m++){
            set_clock_config(clock_config, m);
        }
    }

    size_t get_num_mboards(void){
        return (*_dev)[DEVICE_PROP_MBOARD_NAMES].as<prop_names_t>().size();
    }

    sensor_value_t get_mboard_sensor(const std::string &name, size_t mboard){
        return _mboard(mboard)[named_prop_t(MBOARD_PROP_SENSOR, name)].as<sensor_value_t>();
    }

    std::vector<std::string> get_mboard_sensor_names(size_t mboard){
        return _mboard(mboard)[MBOARD_PROP_SENSOR_NAMES].as<prop_names_t>();
    }
    
    mboard_iface::sptr get_mboard_iface(size_t mboard){
        return _mboard(mboard)[MBOARD_PROP_IFACE].as<mboard_iface::sptr>();
    }

    /*******************************************************************
     * RX methods
     ******************************************************************/
    void set_rx_subdev_spec(const subdev_spec_t &spec, size_t mboard){
        if (mboard != ALL_MBOARDS){
            _mboard(mboard)[MBOARD_PROP_RX_SUBDEV_SPEC] = spec;
            return;
        }
        for (size_t m = 0; m < get_num_mboards(); m++){
            set_rx_subdev_spec(spec, m);
        }
    }

    subdev_spec_t get_rx_subdev_spec(size_t mboard){
        return _mboard(mboard)[MBOARD_PROP_RX_SUBDEV_SPEC].as<subdev_spec_t>();
    }

    size_t get_rx_num_channels(void){
        size_t sum = 0;
        for (size_t m = 0; m < get_num_mboards(); m++){
            sum += get_rx_subdev_spec(m).size();
        }
        return sum;
    }

    std::string get_rx_subdev_name(size_t chan){
        return _rx_subdev(chan)[SUBDEV_PROP_NAME].as<std::string>();
    }

    void set_rx_rate(double rate, size_t chan){
        if (chan != ALL_CHANS){
            _rx_dsp(chan)[DSP_PROP_HOST_RATE] = rate;
            do_samp_rate_warning_message(rate, get_rx_rate(chan), "RX");
            return;
        }
        for (size_t c = 0; c < get_rx_num_channels(); c++){
            set_rx_rate(rate, c);
        }
    }

    double get_rx_rate(size_t chan){
        return _rx_dsp(chan)[DSP_PROP_HOST_RATE].as<double>();
    }

    tune_result_t set_rx_freq(const tune_request_t &tune_request, size_t chan){
        tune_result_t r = tune_rx_subdev_and_dsp(_rx_subdev(chan), _rx_dsp(chan), tune_request);
        do_tune_freq_warning_message(tune_request.target_freq, get_rx_freq(chan), "RX");
        return r;
    }

    double get_rx_freq(size_t chan){
        return derive_freq_from_rx_subdev_and_dsp(_rx_subdev(chan), _rx_dsp(chan));
    }

    freq_range_t get_rx_freq_range(size_t chan){
        return add_dsp_shift(_rx_subdev(chan)[SUBDEV_PROP_FREQ_RANGE].as<freq_range_t>(), _rx_dsp(chan));
    }

    void set_rx_gain(double gain, const std::string &name, size_t chan){
        return _rx_gain_group(chan)->set_value(gain, name);
    }

    double get_rx_gain(const std::string &name, size_t chan){
        return _rx_gain_group(chan)->get_value(name);
    }

    gain_range_t get_rx_gain_range(const std::string &name, size_t chan){
        return _rx_gain_group(chan)->get_range(name);
    }

    std::vector<std::string> get_rx_gain_names(size_t chan){
        return _rx_gain_group(chan)->get_names();
    }

    void set_rx_antenna(const std::string &ant, size_t chan){
        _rx_subdev(chan)[SUBDEV_PROP_ANTENNA] = ant;
    }

    std::string get_rx_antenna(size_t chan){
        return _rx_subdev(chan)[SUBDEV_PROP_ANTENNA].as<std::string>();
    }

    std::vector<std::string> get_rx_antennas(size_t chan){
        return _rx_subdev(chan)[SUBDEV_PROP_ANTENNA_NAMES].as<prop_names_t>();
    }

    void set_rx_bandwidth(double bandwidth, size_t chan){
        _rx_subdev(chan)[SUBDEV_PROP_BANDWIDTH] = bandwidth;
    }

    double get_rx_bandwidth(size_t chan){
        return _rx_subdev(chan)[SUBDEV_PROP_BANDWIDTH].as<double>();
    }

    dboard_iface::sptr get_rx_dboard_iface(size_t chan){
        return _rx_dboard(chan)[DBOARD_PROP_DBOARD_IFACE].as<dboard_iface::sptr>();
    }

    sensor_value_t get_rx_sensor(const std::string &name, size_t chan){
        return _rx_subdev(chan)[named_prop_t(SUBDEV_PROP_SENSOR, name)].as<sensor_value_t>();
    }

    std::vector<std::string> get_rx_sensor_names(size_t chan){
        return _rx_subdev(chan)[SUBDEV_PROP_SENSOR_NAMES].as<prop_names_t>();
    }

    /*******************************************************************
     * TX methods
     ******************************************************************/
    void set_tx_subdev_spec(const subdev_spec_t &spec, size_t mboard){
        if (mboard != ALL_MBOARDS){
            _mboard(mboard)[MBOARD_PROP_TX_SUBDEV_SPEC] = spec;
            return;
        }
        for (size_t m = 0; m < get_num_mboards(); m++){
            set_tx_subdev_spec(spec, m);
        }
    }

    subdev_spec_t get_tx_subdev_spec(size_t mboard){
        return _mboard(mboard)[MBOARD_PROP_TX_SUBDEV_SPEC].as<subdev_spec_t>();
    }

    std::string get_tx_subdev_name(size_t chan){
        return _tx_subdev(chan)[SUBDEV_PROP_NAME].as<std::string>();
    }

    size_t get_tx_num_channels(void){
        size_t sum = 0;
        for (size_t m = 0; m < get_num_mboards(); m++){
            sum += get_tx_subdev_spec(m).size();
        }
        return sum;
    }

    void set_tx_rate(double rate, size_t chan){
        if (chan != ALL_CHANS){
            _tx_dsp(chan)[DSP_PROP_HOST_RATE] = rate;
            do_samp_rate_warning_message(rate, get_tx_rate(chan), "TX");
            return;
        }
        for (size_t c = 0; c < get_tx_num_channels(); c++){
            set_tx_rate(rate, c);
        }
    }

    double get_tx_rate(size_t chan){
        return _tx_dsp(chan)[DSP_PROP_HOST_RATE].as<double>();
    }

    tune_result_t set_tx_freq(const tune_request_t &tune_request, size_t chan){
        tune_result_t r = tune_tx_subdev_and_dsp(_tx_subdev(chan), _tx_dsp(chan), tune_request);
        do_tune_freq_warning_message(tune_request.target_freq, get_tx_freq(chan), "TX");
        return r;
    }

    double get_tx_freq(size_t chan){
        return derive_freq_from_tx_subdev_and_dsp(_tx_subdev(chan), _tx_dsp(chan));
    }

    freq_range_t get_tx_freq_range(size_t chan){
        return add_dsp_shift(_tx_subdev(chan)[SUBDEV_PROP_FREQ_RANGE].as<freq_range_t>(), _tx_dsp(chan));
    }

    void set_tx_gain(double gain, const std::string &name, size_t chan){
        return _tx_gain_group(chan)->set_value(gain, name);
    }

    double get_tx_gain(const std::string &name, size_t chan){
        return _tx_gain_group(chan)->get_value(name);
    }

    gain_range_t get_tx_gain_range(const std::string &name, size_t chan){
        return _tx_gain_group(chan)->get_range(name);
    }

    std::vector<std::string> get_tx_gain_names(size_t chan){
        return _tx_gain_group(chan)->get_names();
    }

    void set_tx_antenna(const std::string &ant, size_t chan){
        _tx_subdev(chan)[SUBDEV_PROP_ANTENNA] = ant;
    }

    std::string get_tx_antenna(size_t chan){
        return _tx_subdev(chan)[SUBDEV_PROP_ANTENNA].as<std::string>();
    }

    std::vector<std::string> get_tx_antennas(size_t chan){
        return _tx_subdev(chan)[SUBDEV_PROP_ANTENNA_NAMES].as<prop_names_t>();
    }

    void set_tx_bandwidth(double bandwidth, size_t chan){
        _tx_subdev(chan)[SUBDEV_PROP_BANDWIDTH] = bandwidth;
    }

    double get_tx_bandwidth(size_t chan){
        return _tx_subdev(chan)[SUBDEV_PROP_BANDWIDTH].as<double>();
    }

    dboard_iface::sptr get_tx_dboard_iface(size_t chan){
        return _tx_dboard(chan)[DBOARD_PROP_DBOARD_IFACE].as<dboard_iface::sptr>();
    }

    sensor_value_t get_tx_sensor(const std::string &name, size_t chan){
        return _tx_subdev(chan)[named_prop_t(SUBDEV_PROP_SENSOR, name)].as<sensor_value_t>();
    }

    std::vector<std::string> get_tx_sensor_names(size_t chan){
        return _tx_subdev(chan)[SUBDEV_PROP_SENSOR_NAMES].as<prop_names_t>();
    }

private:
    device::sptr _dev;

    struct mboard_chan_pair{
        size_t mboard, chan;
        mboard_chan_pair(void): mboard(0), chan(0){}
    };

    mboard_chan_pair rx_chan_to_mcp(size_t chan){
        mboard_chan_pair mcp;
        mcp.chan = chan;
        for (mcp.mboard = 0; mcp.mboard < get_num_mboards(); mcp.mboard++){
            size_t sss = get_rx_subdev_spec(mcp.mboard).size();
            if (mcp.chan < sss) break;
            mcp.chan -= sss;
        }
        return mcp;
    }

    mboard_chan_pair tx_chan_to_mcp(size_t chan){
        mboard_chan_pair mcp;
        mcp.chan = chan;
        for (mcp.mboard = 0; mcp.mboard < get_num_mboards(); mcp.mboard++){
            size_t sss = get_tx_subdev_spec(mcp.mboard).size();
            if (mcp.chan < sss) break;
            mcp.chan -= sss;
        }
        return mcp;
    }

    wax::obj _mboard(size_t mboard){
        std::string mb_name = (*_dev)[DEVICE_PROP_MBOARD_NAMES].as<prop_names_t>().at(mboard);
        return (*_dev)[named_prop_t(DEVICE_PROP_MBOARD, mb_name)];
    }
    wax::obj _rx_dsp(size_t chan){
        mboard_chan_pair mcp = rx_chan_to_mcp(chan);
        prop_names_t dsp_names = _mboard(mcp.mboard)[MBOARD_PROP_RX_DSP_NAMES].as<prop_names_t>();
        return _mboard(mcp.mboard)[named_prop_t(MBOARD_PROP_RX_DSP, dsp_names.at(mcp.chan))];
    }
    wax::obj _tx_dsp(size_t chan){
        mboard_chan_pair mcp = tx_chan_to_mcp(chan);
        prop_names_t dsp_names = _mboard(mcp.mboard)[MBOARD_PROP_TX_DSP_NAMES].as<prop_names_t>();
        return _mboard(mcp.mboard)[named_prop_t(MBOARD_PROP_TX_DSP, dsp_names.at(mcp.chan))];
    }
    wax::obj _rx_dboard(size_t chan){
        mboard_chan_pair mcp = rx_chan_to_mcp(chan);
        std::string db_name = get_rx_subdev_spec(mcp.mboard).at(mcp.chan).db_name;
        return _mboard(mcp.mboard)[named_prop_t(MBOARD_PROP_RX_DBOARD, db_name)];
    }
    wax::obj _tx_dboard(size_t chan){
        mboard_chan_pair mcp = tx_chan_to_mcp(chan);
        std::string db_name = get_tx_subdev_spec(mcp.mboard).at(mcp.chan).db_name;
        return _mboard(mcp.mboard)[named_prop_t(MBOARD_PROP_TX_DBOARD, db_name)];
    }
    wax::obj _rx_subdev(size_t chan){
        mboard_chan_pair mcp = rx_chan_to_mcp(chan);
        std::string sd_name = get_rx_subdev_spec(mcp.mboard).at(mcp.chan).sd_name;
        return _rx_dboard(chan)[named_prop_t(DBOARD_PROP_SUBDEV, sd_name)];
    }
    wax::obj _tx_subdev(size_t chan){
        mboard_chan_pair mcp = tx_chan_to_mcp(chan);
        std::string sd_name = get_tx_subdev_spec(mcp.mboard).at(mcp.chan).sd_name;
        return _tx_dboard(chan)[named_prop_t(DBOARD_PROP_SUBDEV, sd_name)];
    }
    gain_group::sptr _rx_gain_group(size_t chan){
        mboard_chan_pair mcp = rx_chan_to_mcp(chan);
        std::string sd_name = get_rx_subdev_spec(mcp.mboard).at(mcp.chan).sd_name;
        return _rx_dboard(chan)[named_prop_t(DBOARD_PROP_GAIN_GROUP, sd_name)].as<gain_group::sptr>();
    }
    gain_group::sptr _tx_gain_group(size_t chan){
        mboard_chan_pair mcp = tx_chan_to_mcp(chan);
        std::string sd_name = get_tx_subdev_spec(mcp.mboard).at(mcp.chan).sd_name;
        return _tx_dboard(chan)[named_prop_t(DBOARD_PROP_GAIN_GROUP, sd_name)].as<gain_group::sptr>();
    }
};

/***********************************************************************
 * The Make Function
 **********************************************************************/
multi_usrp::sptr multi_usrp::make(const device_addr_t &dev_addr){
    return sptr(new multi_usrp_impl(dev_addr));
}
