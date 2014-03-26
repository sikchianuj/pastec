#include <iostream>
#include <set>

#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>

#include "imagefeatureextractor.h"
#include "clientconnection.h"
#include "dataMessages.h"


ImageFeatureExtractor::ImageFeatureExtractor(String visualWordsPath, String indexPath)
    : visualWordsPath(visualWordsPath), indexPath(indexPath)
{ }


void ImageFeatureExtractor::init()
{
    words = new Mat(0, 128, CV_32F); // The matrix that stores the visual words.

    if (!readVisualWords(visualWordsPath))
        exit(1);
    assert(words->rows == 1000000);

    cout << "Building the kd-trees." << endl;
    //index = new flann::Index(*words, flann::KDTreeIndexParams());
    index = new flann::Index(*words, flann::SavedIndexParams(indexPath));
}


void ImageFeatureExtractor::stop()
{
    // TODO: add a mutex to be sure that we can kill this module.
    delete words;
    delete index;
}


bool ImageFeatureExtractor::processNewImage(unsigned i_imageId, unsigned i_imgSize,
                                     char *p_imgData, ClientConnection *p_client)
{
    vector<char> imgData(i_imgSize);
    memcpy(imgData.data(), p_imgData, i_imgSize);

    Mat img;
    try
    {
        img = imdecode(imgData, CV_LOAD_IMAGE_GRAYSCALE);
    }
    catch (cv::Exception& e) // The decoding of an image can raise an exception.
    {
        const char* err_msg = e.what();
        cout << "Exception caught: " << err_msg << endl;
        p_client->sendReply(IMAGE_NOT_DECODED);
        return false;
    }

    if (!img.data)
    {
        cout << "Error reading the image." << std::endl;
        p_client->sendReply(IMAGE_NOT_DECODED);
        return false;
    }

    unsigned i_imgWidth = img.cols;
    unsigned i_imgHeight = img.rows;


    if (i_imgWidth > 1000
        || i_imgHeight > 1000)
    {
        cout << "Image too large." << endl;
        p_client->sendReply(IMAGE_SIZE_TOO_BIG);
        return false;
    }

#if 1
    if (i_imgWidth < 200
        || i_imgHeight < 200)
    {
        cout << "Image too small." << endl;
        p_client->sendReply(IMAGE_SIZE_TOO_SMALL);
        return false;
    }
#endif

    vector<KeyPoint> keypoints;
    Mat descriptors;

    SIFT()(img, noArray(), keypoints, descriptors);

    // Output the SIFT keypoints to the database.

    // TODO: create the imageHits directory if it does not exist.

    ofstream ofs;
    if (!openHitFile(ofs, i_imageId))
    {
        p_client->sendReply(ERROR_GENERIC);
        return false;
    }

    unsigned i_nbKeyPoints = 0;
    vector<KeyPoint> cleanKeypoints;
    for (unsigned i = 0; i < keypoints.size(); ++i)
    {
        cleanKeypoints.push_back(keypoints[i]);
        i_nbKeyPoints++;

        // Recording the angle on 16 bits.
        u_int16_t angle = keypoints[i].angle / 360 * (1 << 16);
        u_int16_t x = keypoints[i].pt.x / i_imgWidth * (1 << 16);
        u_int16_t y = keypoints[i].pt.y / i_imgHeight * (1 << 16);

        vector<int> indices(4);
        vector<float> dists(4);
        index->knnSearch(descriptors.row(i), indices, dists, 4);

        for (unsigned j = 0; j < indices.size(); ++j)
        {
            HitForward newHit;
            newHit.i_wordId = indices[j];
            newHit.i_imageId = i_imageId;
            newHit.i_angle = angle;
            newHit.x = x;
            newHit.y = y;
            // Write the hit in the file.
            if (!writeHit(ofs, newHit))
            {
                ofs.close();
                p_client->sendReply(ERROR_GENERIC);
                return false;
            }
        }
    }

    ofs.close();
    std::cout << "Nb SIFTs: " << i_nbKeyPoints << std::endl;

#if 0
    // Draw keypoints.
    Mat img_res;
    drawKeypoints(img, keypoints, img_res, Scalar::all(-1), DrawMatchesFlags::DEFAULT);

    // Show the image.
    imshow("Keypoints 1", img_res);
    waitKey();
#endif

    p_client->sendReply(OK);

    return true;
}


/**
 * @brief Open the file that will contain all hits of the image.
 * @param i_imageId the image id.
 * @return true on success else false.
 */
bool ImageFeatureExtractor::openHitFile(ofstream &ofs, unsigned i_imageId)
{
    stringstream fileNameStream;
    fileNameStream << "imageHits/" << i_imageId << ".dat";

    ofs.open(fileNameStream.str().c_str(), ios_base::binary);

    if (!ofs.good())
    {
        cout << "Could not open the hit output file." << endl;
        ofs.close();
        return false;
    }

    return true;
}


/**
 * @brief Write a new hit in the file.
 * @param hit the new hit to write.
 * @param ofs the output file stream.
 * @return true on success else false.
 */
bool ImageFeatureExtractor::writeHit(ofstream &ofs, HitForward hit)
{
    if (!ofs.good())
    {
        cout << "Could not write to the output file." << endl;
        return false;
    }

    ofs.write((char *)&hit.i_wordId, sizeof(u_int32_t));
    ofs.write((char *)&hit.i_imageId, sizeof(u_int32_t));
    ofs.write((char *)&hit.i_angle, sizeof(u_int16_t));
    ofs.write((char *)&hit.x, sizeof(u_int16_t));
    ofs.write((char *)&hit.y, sizeof(u_int16_t));

    return true;
}


/**
 * @brief Read the list of visual words from an external file.
 * @param fileName the path of the input file name.
 * @param words a pointer to a matrix to store the words.
 * @return true on success else false.
 */
bool ImageFeatureExtractor::readVisualWords(string fileName)
{
    cout << "Reading the visual words file." << endl;

    // Open the input file.
    ifstream ifs;
    ifs.open(fileName.c_str(), ios_base::binary);

    if (!ifs.good())
    {
        cout << "Could not open the input file." << endl;
        return false;
    }

    float c;
    while (ifs.good())
    {
        Mat line(1, 128, CV_32F);
        for (unsigned i_col = 0; i_col < 128; ++i_col)
        {
            ifs.read((char *)&c, sizeof(float));
            line.at<float>(0, i_col) = c;
        }
        if (!ifs.good())
            break;
        words->push_back(line);
        ifs.ignore(numeric_limits<streamsize>::max(), '\n');
    }

    ifs.close();

    return true;
}
