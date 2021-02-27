""" Module for reading the MPD file
    Author: Parikshit Juluri
    Contact : pjuluri@umkc.edu
"""
from __future__ import division
import re
import config_dash
import pymongo

# Dictionary to convert size to bits
SIZE_DICT = {'bits': 1,
             'Kbits': 1024,
             'Mbits': 1024 * 1024,
             'bytes': 8,
             'KB': 1024 * 8,
             'MB': 1024 * 1024 * 8,
             }
# Try to import the C implementation of ElementTree which is faster
# In case of ImportError import the pure Python implementation
try:
    import xml.etree.cElementTree as ET
except ImportError:
    import xml.etree.ElementTree as ET

MEDIA_PRESENTATION_DURATION = 'mediaPresentationDuration'
MIN_BUFFER_TIME = 'minBufferTime'


def get_tag_name(xml_element):
    """ Module to remove the xmlns tag from the name
        eg: '{urn:mpeg:dash:schema:mpd:2011}SegmentTemplate'
             Return: SegmentTemplate
    """
    try:
        tag_name = xml_element[xml_element.find('}') + 1:]
    except TypeError:
        print("Unable to retrieve the tag. ")
        return None
    return tag_name


def get_playback_time(playback_duration):
    """ Get the playback time(in seconds) from the string:
        Eg: PT0H1M59.89S
    """
    # Get all the numbers in the string
    numbers = re.split('[PTHMS]', playback_duration)
    # remove all the empty strings
    numbers = [value for value in numbers if value != '']
    numbers.reverse()
    total_duration = 0
    for count, val in enumerate(numbers):
        if count == 0:
            total_duration += float(val)
        elif count == 1:
            total_duration += float(val) * 60
        elif count == 2:
            total_duration += float(val) * 60 * 60
    return total_duration


class MediaObject(object):
    """Object to handel audio and video stream """

    def __init__(self):
        self.min_buffer_time = None
        self.start = None
        self.timescale = None
        self.segment_duration = None
        self.initialization = None
        self.base_url = None
        self.segment_sizes = list()
        self.url_list = list()
        self.bandwidth = None


class DashPlayback:
    """
    Audio[bandwidth] : {duration, url_list}
    Video[bandwidth] : {duration, url_list}
    """

    def __init__(self):
        self.min_buffer_time = None
        self.playback_duration = None
        self.audio = dict()
        self.video = dict()


def get_url_list(media, segment_duration, playback_duration, bitrate):
    """
    Module to get the List of URLs
    """
    # Counting the init file
    total_playback = segment_duration
    segment_count = media.start
    # Get the Base URL string
    base_url = media.base_url
    if "$Bandwidth$" in base_url:
        base_url = base_url.replace("$Bandwidth$", str(bitrate))
    if "$Number" in base_url:
        base_url = base_url.split('$')
        base_url[1] = base_url[1].replace('$', '')
        base_url[1] = base_url[1].replace('Number', '')
        base_url = ''.join(base_url)
    while True:
        media.url_list.append(base_url % segment_count)
        segment_count += 1
        if total_playback > playback_duration:
            break
        total_playback += segment_duration
    return media


def read_mpd(mpd_file):
    try:
        client = pymongo.MongoClient()
        print("Connected successfully again!!!")
    except pymongo.errors.ConnectionFailure as e:
        print("Could not connect to MongoDB sadly: %s" % e)
    db = client.cachestatus
    table = db.mpdinfo

    """ Module to read the MPD file"""
    url = []
    print("Reading the MPD file")
    try:
        tree = ET.parse(mpd_file)
    except IOError:
        print("MPD file not found. Exiting")
        return None
    config_dash.JSON_HANDLE["video_metadata"] = {'mpd_file': mpd_file}
    root = tree.getroot()
    dashplayback = DashPlayback()
    if 'MPD' in get_tag_name(root.tag).upper():
        if MEDIA_PRESENTATION_DURATION in root.attrib:
            dashplayback.playback_duration = get_playback_time(root.attrib[MEDIA_PRESENTATION_DURATION])
        if MIN_BUFFER_TIME in root.attrib:
            dashplayback.min_buffer_time = get_playback_time(root.attrib[MIN_BUFFER_TIME])
    # print("-----------MPD PARSER BASEURL------------- %s \n -----------------"%root[0])
    b_period = root[0]
    for b_url in b_period:
        if 'url' in b_url.attrib:
            url.append(b_url.attrib['url'])
    video_segment_duration = None
    media_info = []
    child_period = root[1]
    for adaptation_set in child_period:
        if 'mimeType' in adaptation_set.attrib:
            media_found = False
        if 'audio' in adaptation_set.attrib['mimeType']:
            media_object = dashplayback.audio
            media_found = False
            print("Found Audio")
        elif 'video' in adaptation_set.attrib['mimeType']:
            media_object = dashplayback.video
            media_found = True
            print("Found Video")
        if media_found:
            print("Retrieving Media")
        config_dash.JSON_HANDLE["video_metadata"]['available_bitrates'] = list()
        for representation in adaptation_set:
            bandwidth = int(representation.attrib['bandwidth'])
            config_dash.JSON_HANDLE["video_metadata"]['available_bitrates'].append(bandwidth)
            media_object[bandwidth] = MediaObject()
            for segment_info in representation:
                if "SegmentTemplate" in get_tag_name(segment_info.tag):
                    media_object[bandwidth].base_url = segment_info.attrib['media']
                    media_object[bandwidth].start = int(segment_info.attrib['startNumber'])
                    media_object[bandwidth].timescale = float(segment_info.attrib['timescale'])
                    media_object[bandwidth].initialization = segment_info.attrib['initialization']
                    urlstring = segment_info.attrib['media']
                    urlstring = urlstring.replace("$Bandwidth$", str(bandwidth))
                    # print urlstring
                if 'video' in adaptation_set.attrib['mimeType']:
                    if "SegmentSize" in get_tag_name(segment_info.tag):
                        try:
                            segment_size = float(segment_info.attrib['size']) * float(
                                SIZE_DICT[segment_info.attrib['scale']])
                        except KeyError as e:
                            print("Error in reading Segment sizes :{}".format(e))
                            continue
                        seg_no = segment_info.attrib['id']
                        # print seg_no
                        seg_no = re.findall(r'\d+', seg_no)
                        seg_no = seg_no[len(seg_no) - 2]
                        # print seg_no

                        if "$Number$%d" in urlstring:
                            firststr = str(seg_no)
                            urlstring = urlstring.replace("$Number$%d", str(seg_no))
                        else:
                            oldstr = "2s" + prev_seg
                            newstr = "2s" + str(seg_no)
                            # print oldstr
                            # print newstr
                            urlstring = urlstring.replace(oldstr, newstr)

                        prev_seg = str(seg_no)
                        # print urlstring
                        media_object[bandwidth].segment_sizes.append(segment_size)
                        post = {"urn": urlstring, "quality": str(bandwidth), "seg_no": int(seg_no),
                                "seg_size": int(segment_size)}
                        # print urlstring
                        mongo_ins = table.replace_one({"urn": urlstring}, post, True)
                    elif "SegmentTemplate" in get_tag_name(segment_info.tag):
                        video_segment_duration = (float(segment_info.attrib['duration']) / float(
                            segment_info.attrib['timescale']))
                        print("Segment Playback Duration = {}".format(video_segment_duration))
        media_info.append(media_object[bandwidth])
        # return media_info
