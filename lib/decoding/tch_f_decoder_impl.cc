/* -*- c++ -*- */
/*
 * @file
 * @author Roman Khassraf <rkhassraf@gmail.com>
 * @section LICENSE
 *
 * Gr-gsm is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * Gr-gsm is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with gr-gsm; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gnuradio/io_signature.h>
#include <grgsm/gsmtap.h>
#include "stdio.h"
#include "tch_f_decoder_impl.h"

#define DATA_BYTES 23

namespace gr {
  namespace gsm {

    tch_f_decoder::sptr
    tch_f_decoder::make(tch_mode mode, const std::string &file)
    {
      return gnuradio::get_initial_sptr
        (new tch_f_decoder_impl(mode, file));
    }

    /*
     * Constructor
     */
    tch_f_decoder_impl::tch_f_decoder_impl(tch_mode mode, const std::string &file)
      : gr::block("tch_f_decoder",
              gr::io_signature::make(0, 0, 0),
              gr::io_signature::make(0, 0, 0)),
      d_tch_mode(mode),
      d_collected_bursts_num(0),
      mBlockCoder(0x10004820009ULL, 40, 224),
      mU(228),
      mP(mU.segment(184,40)),
      mD(mU.head(184)),
      mDP(mU.head(224)),
      mC(CONV_SIZE),
      mClass1_c(mC.head(378)),
      mClass2_c(mC.segment(378, 78)),
      mTCHU(189),
      mTCHD(260),
      mClass1A_d(mTCHD.head(50)),
      mTCHParity(0x0b, 3, 50)
    {
        d_speech_file = fopen( file.c_str(), "wb" );
        if (d_speech_file == NULL)
        {
            throw std::runtime_error("TCH/F Decoder: can't open file\n");
        }

        if (d_tch_mode != TCH_FS)
        {
            fwrite(amr_nb_magic, 1, 6, d_speech_file);
        }

        int j, k, B;
        for (k = 0; k < CONV_SIZE; k++)
        {
            B = k % 8;
            j = 2 * ((49 * k) % 57) + ((k % 8) / 4);
            interleave_trans[k] = B * 114 + j;
        }

        //setup input/output ports
        message_port_register_in(pmt::mp("bursts"));
        set_msg_handler(pmt::mp("bursts"), boost::bind(&tch_f_decoder_impl::decode, this, _1));
        message_port_register_out(pmt::mp("msgs"));

        setCodingMode(mode);
    }

    tch_f_decoder_impl::~tch_f_decoder_impl()
    {
    }

    void tch_f_decoder_impl::decode(pmt::pmt_t msg)
    {
        d_bursts[d_collected_bursts_num] = msg;
        d_collected_bursts_num++;

        bool stolen = false;

        if (d_collected_bursts_num == 8)
        {
            d_collected_bursts_num = 0;

            // reorganize data
            for (int ii = 0; ii < 8; ii++)
            {
                pmt::pmt_t header_plus_burst = pmt::cdr(d_bursts[ii]);
                int8_t * burst_bits = (int8_t *)(pmt::blob_data(header_plus_burst))+sizeof(gsmtap_hdr);

                for (int jj = 0; jj < 57; jj++)
                {
                    iBLOCK[ii*114+jj] = burst_bits[jj + 3];
                    iBLOCK[ii*114+jj+57] = burst_bits[jj + 88]; //88 = 3+57+1+26+1
                }

                if ((ii <= 3 && static_cast<int>(burst_bits[87]) == 1) || (ii >= 4 && static_cast<int>(burst_bits[60]) == 1))
                {
                    stolen = true;
                }
            }

            // deinterleave
            for (int k = 0; k < CONV_SIZE; k++)
            {
                mC[k] = iBLOCK[interleave_trans[k]];
            }

            // Decode stolen frames as FACCH/F
            if (stolen)
            {
                mVR204Coder.decode(mC, mU);
                mP.invert();

                unsigned syndrome = mBlockCoder.syndrome(mDP);

                if (syndrome == 0)
                {
                    unsigned char outmsg[27];
                    unsigned char sbuf_len=224;
                    int i, j, c, pos=0;
                    for(i = 0; i < sbuf_len; i += 8) {
                        for(j = 0, c = 0; (j < 8) && (i + j < sbuf_len); j++){
                            c |= (!!mU.bit(i + j)) << j;
                        }
                        outmsg[pos++] = c & 0xff;
                    }

                    pmt::pmt_t first_header_plus_burst = pmt::cdr(d_bursts[0]);
                    gsmtap_hdr * header = (gsmtap_hdr *)pmt::blob_data(first_header_plus_burst);
                    header->type = GSMTAP_TYPE_UM;
                    int8_t * header_content = (int8_t *)header;
                    int8_t header_plus_data[sizeof(gsmtap_hdr)+DATA_BYTES];
                    memcpy(header_plus_data, header_content, sizeof(gsmtap_hdr));
                    memcpy(header_plus_data+sizeof(gsmtap_hdr), outmsg, DATA_BYTES);

                    pmt::pmt_t msg_binary_blob = pmt::make_blob(header_plus_data,DATA_BYTES+sizeof(gsmtap_hdr));
                    pmt::pmt_t msg_out = pmt::cons(pmt::PMT_NIL, msg_binary_blob);

                    message_port_pub(pmt::mp("msgs"), msg_out);
                }
            }

            // Decode voice frames and write to file
            if (d_tch_mode == TCH_FS || d_tch_mode == TCH_EFR)
            {
                mVR204Coder.decode(mClass1_c, mTCHU);
                mClass2_c.sliced().copyToSegment(mTCHD, 182);

                // 3.1.2.1
                // copy class 1 bits u[] to d[]
                for (unsigned k = 0; k <= 90; k++) {
                  mTCHD[2*k] = mTCHU[k];
                  mTCHD[2*k+1] = mTCHU[184-k];
                }

                // 3.1.2.1
                // check parity of class 1A
                unsigned sentParity = (~mTCHU.peekField(91, 3)) & 0x07;
                unsigned calcParity = mClass1A_d.parity(mTCHParity) & 0x07;

                bool good = (sentParity == calcParity);

                if (good)
                {
                    unsigned char frameBuffer[33];
                    unsigned int  mTCHFrameLength;

                    if (d_tch_mode == TCH_FS) // GSM-FR
                    {
                        unsigned char mFrameHeader = 0x0d;
                        mTCHFrameLength = 33;

                        // Undo Um's importance-sorted bit ordering.
                        // See GSM 05.03 3.1 and Table 2.
                        BitVector frFrame(260 + 4); // FR has a frameheader of 4 bits only
                        BitVector payload = frFrame.tail(4);

                        mTCHD.unmap(GSM::g610BitOrder, 260, payload);
                        frFrame.pack(frameBuffer);

                    }
                    else if (d_tch_mode == TCH_EFR) // GSM-EFR
                    {
                        unsigned char mFrameHeader = 0x3c;

                        // AMR Frame, consisting of a 8 bit frame header, plus the payload from decoding
                        BitVector amrFrame(244 + 8); // Same output length as AMR 12.2
                        BitVector payload = amrFrame.tail(8);

                        BitVector TCHW(260), EFRBits(244);

                        // write frame header
                        amrFrame.fillField(0, mFrameHeader, 8);

                        // Undo Um's EFR bit ordering.
                        mTCHD.unmap(GSM::g660BitOrder, 260, TCHW);

                        // Remove repeating bits and CRC to get raw EFR frame (244 bits)
                        for (unsigned k=0; k<71; k++)
                          EFRBits[k] = TCHW[k] & 1;

                        for (unsigned k=73; k<123; k++)
                          EFRBits[k-2] = TCHW[k] & 1;

                        for (unsigned k=125; k<178; k++)
                          EFRBits[k-4] = TCHW[k] & 1;

                        for (unsigned k=180; k<230; k++)
                          EFRBits[k-6] = TCHW[k] & 1;

                        for (unsigned k=232; k<252; k++)
                          EFRBits[k-8] = TCHW[k] & 1;

                        // Map bits as AMR 12.2k
                        EFRBits.map(GSM::gAMRBitOrderTCH_AFS12_2, 244, payload);

                        // Put the whole frame (hdr + payload)
                        mTCHFrameLength = 32;
                        amrFrame.pack(frameBuffer);

                    }
                    fwrite(frameBuffer, 1 , mTCHFrameLength, d_speech_file);
                }
            }
            else
            {
                // Handle inband bits, see 3.9.4.1
                // OpenBTS source takes last 8 bits as inband bits for some reason. This may be either a
                // divergence between their implementation and GSM specification, which works because
                // both their encoder and decoder do it same way, or they handle the issue at some other place
                // SoftVector cMinus8 = mC.segment(0, mC.size() - 8);
                SoftVector cMinus8 = mC.segment(8, mC.size());
                cMinus8.copyUnPunctured(mTCHUC, mPuncture, mPunctureLth);

                // 3.9.4.4
                // decode from uc[] to u[]
                mViterbi->decode(mTCHUC, mTCHU);

                // 3.9.4.3 -- class 1a bits in u[] to d[]
                for (unsigned k=0; k < mClass1ALth; k++) {
                    mTCHD[k] = mTCHU[k];
                }

                // 3.9.4.3 -- class 1b bits in u[] to d[]
                for (unsigned k=0; k < mClass1BLth; k++) {
                    mTCHD[k+mClass1ALth] = mTCHU[k+mClass1ALth+6];
                }

                // Check parity
                unsigned sentParity = (~mTCHU.peekField(mClass1ALth,6)) & 0x3f;
                BitVector class1A = mTCHU.segment(0, mClass1ALth);
                unsigned calcParity = class1A.parity(mTCHParity) & 0x3f;

                bool good = (sentParity == calcParity);

                if (good)
                {
                    unsigned char frameBuffer[mAMRFrameLth];
                    // AMR Frame, consisting of a 8 bit frame header, plus the payload from decoding
                    BitVector amrFrame(mKd + 8);
                    BitVector payload = amrFrame.tail(8);

                    // write frame header
                    amrFrame.fillField(0, mAMRFrameHeader, 8);

                    // We don't unmap here, but copy the decoded bits directly
                    // Decoder already delivers correct bit order
                    // mTCHD.unmap(mAMRBitOrder, payload.size(), payload);
                    mTCHD.copyTo(payload);
                    amrFrame.pack(frameBuffer);
                    fwrite(frameBuffer, 1 , mAMRFrameLth, d_speech_file);
                }
            }
        }
    }

    void tch_f_decoder_impl::setCodingMode(tch_mode mode)
    {
        if (d_tch_mode  != TCH_FS && d_tch_mode != TCH_EFR)
        {
            mKd = GSM::gAMRKd[d_tch_mode];
            mTCHD.resize(mKd);
            mTCHU.resize(mKd+6);
            mTCHParity = Parity(0x06f,6, GSM::gAMRClass1ALth[d_tch_mode]);
            mAMRBitOrder = GSM::gAMRBitOrder[d_tch_mode];
            mClass1ALth = GSM::gAMRClass1ALth[d_tch_mode];
            mClass1BLth = GSM::gAMRKd[d_tch_mode] - GSM::gAMRClass1ALth[d_tch_mode];
            mTCHUC.resize(GSM::gAMRTCHUCLth[d_tch_mode]);
            mPuncture = GSM::gAMRPuncture[d_tch_mode];
            mPunctureLth = GSM::gAMRPunctureLth[d_tch_mode];
            mClass1A_d.dup(mTCHD.head(mClass1ALth));

            switch (d_tch_mode)
            {
                case TCH_AFS12_2:
                    mViterbi = new ViterbiTCH_AFS12_2();
                    mAMRFrameLth = 32;
                    mAMRFrameHeader = 0x3c;
                    break;
                case TCH_AFS10_2:
                    mViterbi = new ViterbiTCH_AFS10_2();
                    mAMRFrameLth = 27;
                    mAMRFrameHeader = 0x3c;
                    break;
                case TCH_AFS7_95:
                    mViterbi = new ViterbiTCH_AFS7_95();
                    mAMRFrameLth = 21;
                    mAMRFrameHeader = 0x3c;
                    break;
                case TCH_AFS7_4:
                    mViterbi = new ViterbiTCH_AFS7_4();
                    mAMRFrameLth = 20;
                    mAMRFrameHeader = 0x3c;
                    break;
                case TCH_AFS6_7:
                    mViterbi = new ViterbiTCH_AFS6_7();
                    mAMRFrameLth = 18;
                    mAMRFrameHeader = 0x3c;
                    break;
                case TCH_AFS5_9:
                    mViterbi = new ViterbiTCH_AFS5_9();
                    mAMRFrameLth = 16;
                    mAMRFrameHeader = 0x14;
                    break;
                case TCH_AFS5_15:
                    mViterbi = new ViterbiTCH_AFS5_15();
                    mAMRFrameLth = 14;
                    mAMRFrameHeader = 0x3c;
                    break;
                case TCH_AFS4_75:
                    mViterbi = new ViterbiTCH_AFS4_75();
                    mAMRFrameLth = 13;
                    mAMRFrameHeader = 0x3c;
                    break;
                default:
                    mViterbi = new ViterbiTCH_AFS12_2();
                    mAMRFrameLth = 32;
                    mAMRFrameHeader = 0x3c;
                    break;
            }
        }
    }
  } /* namespace gsm */
} /* namespace gr */

