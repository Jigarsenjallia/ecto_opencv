#include <ecto/ecto.hpp>
#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <objcog/db/couch.hpp>
#include <boost/serialization/binary_object.hpp>
#include <boost/serialization/split_free.hpp>
#include <boost/format.hpp>
#include <string>
using ecto::tendrils;

namespace boost
{
  namespace serialization
  {
    template<class Archive>
    void save(Archive & ar, const cv::Mat & m, const unsigned int version)
    {
      int type = m.type();
      ar & m.rows;
      ar & m.cols;
      ar & type;
      const uchar * data = m.data, *end = m.dataend;
      ar & boost::serialization::make_binary_object(const_cast<uchar*> (data),
                                                    size_t(end - data));
    }

    template<class Archive>
    void load(Archive & ar, cv::Mat & m, const unsigned int version)
    {
      int rows, cols, type;
      ar & rows;
      ar & cols;
      ar & type;
      if (rows > 0 && cols > 0)
        {
          m.create(rows, cols, type);
          uchar * data = m.data, *end = m.dataend;
          ar & boost::serialization::make_binary_object(data, end - data);
        }
      else
        {
          std::cout << "bad matrix" << std::endl;
        }
    }
  } // namespace serialization
} // namespace boost

BOOST_SERIALIZATION_SPLIT_FREE( cv::Mat)

void png_attach(cv::Mat image, couch::Document& doc, const std::string& name)
{
  std::vector<uint8_t> buffer;
  std::stringstream ss;
  cv::imencode(".png", image, buffer);
  std::copy(buffer.begin(), buffer.end(), std::ostream_iterator<uint8_t>(ss));
  doc.attach(name, ss, "image/png");
}

void get_png_attachment(cv::Mat& image,
                        couch::Document& doc,
                        const std::string& name)
{
  std::stringstream ss;
  doc.get_attachment_stream(name, ss);
  std::streampos length = ss.tellp();
  std::vector<uint8_t> buffer(length);
  ss.read((char*) buffer.data(), length);
  image = cv::imdecode(buffer, CV_LOAD_IMAGE_ANYDEPTH | CV_LOAD_IMAGE_ANYCOLOR);
}

namespace db
{

  struct Observation
  {
    cv::Mat image, depth, mask;
    cv::Mat R, T, K;
    std::string object_id;
    int frame_number;
    void operator>>(couch::Document& doc)
    {
      doc.update();
      png_attach(image, doc, "image");
      png_attach(depth, doc, "depth");
      png_attach(mask, doc, "mask");
      doc.attach("R", R);
      doc.attach("T", T);
      doc.attach("K", K);
      doc.update();
      doc.set_value("object_id", object_id);
      doc.set_value("frame_number", frame_number);
      doc.commit();
    }

    void operator>>(ecto::tendrils& o)
    {
      o.get<cv::Mat> ("image") = image;
      o.get<cv::Mat> ("mask") = mask;
      o.get<cv::Mat> ("depth") = depth;
      o.get<cv::Mat> ("R") = R;
      o.get<cv::Mat> ("T") = T;
      o.get<cv::Mat> ("K") = K;
      o.get<int> ("frame_number") = frame_number;
    }
    void operator<<(couch::Document& doc)
    {
      doc.update();
      object_id = doc.get_value<std::string> ("object_id");
      frame_number = doc.get_value<int> ("frame_number");

      get_png_attachment(image, doc, "image");
      get_png_attachment(depth, doc, "depth");
      get_png_attachment(mask, doc, "mask");

      doc.get_attachment("R", R);
      doc.get_attachment("T", T);
      doc.get_attachment("K", K);
    }
  };
  struct ObservationInserter
  {
    static void declare_params(tendrils& params)
    {
      params.declare<std::string> (
                                   "object_id",
                                   "The object id, to associate this frame with.",
                                   "object_01");
    }

    static void declare_io(const tendrils& params, tendrils& inputs,
                           tendrils& outputs)
    {
      inputs.declare<cv::Mat> ("image", "An rgb full frame image.");
      inputs.declare<cv::Mat> ("depth", "The 16bit depth image.");
      inputs.declare<cv::Mat> ("mask", "The mask.");
      inputs.declare<cv::Mat> ("R", "The orientation.");
      inputs.declare<cv::Mat> ("T", "The translation.");
      inputs.declare<cv::Mat> ("K", "The camera intrinsic matrix");
      inputs.declare<int> ("trigger", "Capture trigger, 'c' for capture.");
    }

    ObservationInserter() :
      db(std::string(DEFAULT_COUCHDB_URL) + "/frames"), frame_number(0)
    {
    }

    void on_object_id_change(const std::string& id)
    {
      SHOW();
      object_id = id;
      std::cout << "object_id = " << id << std::endl;
    }
    void configure(tendrils& params)
    {
      params.at("object_id").set_callback<std::string> (
                                                        boost::bind(
                                                                    &ObservationInserter::on_object_id_change,
                                                                    this, _1));
      db.create();
      on_object_id_change(params.get<std::string> ("object_id"));

    }
    int process(const tendrils& inputs, tendrils& outputs)
    {
      if (inputs.get<int> ("trigger") != 'c')
        return 0;
      std::cout << "Inserting" << std::endl;
      Observation obj;
      obj.image = inputs.get<cv::Mat> ("image");
      obj.depth = inputs.get<cv::Mat> ("depth");
      obj.mask = inputs.get<cv::Mat> ("mask");
      obj.R = inputs.get<cv::Mat> ("R");
      obj.T = inputs.get<cv::Mat> ("T");
      obj.K = inputs.get<cv::Mat> ("K");
      obj.frame_number = frame_number;
      obj.object_id = object_id;
      couch::Document doc(db);
      doc.create();
      obj >> doc;
      frame_number++;
      return 0;
    }
    couch::Db db;
    int frame_number;
    std::string object_id;
  };
#define STRINGYFY(A) #A

  std::string where_doc_id = STRINGYFY(
      function(doc)
        {
          if(doc.object_id == "%s" )
          emit(null,null);
        }
  );
  struct ObservationReader
  {
    static void declare_params(tendrils& params)
    {
      params.declare<std::string> (
                                   "object_id",
                                   "The object id, to associate this frame with.",
                                   "object_01");
    }

    static void declare_io(const tendrils& params, const tendrils& inputs,
                           tendrils& outputs)
    {
      outputs.declare<cv::Mat> ("image", "An rgb full frame image.");
      outputs.declare<cv::Mat> ("depth", "The 16bit depth image.");
      outputs.declare<cv::Mat> ("mask", "The mask.");
      outputs.declare<cv::Mat> ("R", "The orientation.");
      outputs.declare<cv::Mat> ("T", "The translation.");
      outputs.declare<cv::Mat> ("K", "The camera intrinsic matrix");
      outputs.declare<int> ("frame_number", "The frame number");
    }
    void on_object_id_change(const std::string& id)
    {
      SHOW();
      std::cout << "object_id = " << id << std::endl;

      couch::View v;
      v.add_map("map", boost::str(boost::format(where_doc_id) % id));
      db.run_view(v, -1, 0, total_rows, offset, docs);
      db.print();
      current_frame = 0;

    }
    ObservationReader() :
       db(std::string(DEFAULT_COUCHDB_URL) + "/frames"), current_frame(0)
     {
      db.create();
     }
    void configure(tendrils& params)
    {
      params.at("object_id").set_callback<std::string> (
                                                        boost::bind(
                                                                    &ObservationReader::on_object_id_change,
                                                                    this, _1));
      std::string id = const_cast<const tendrils&>(params).get<std::string>("object_id");
      on_object_id_change(id);
    }
    int process(const tendrils& inputs, tendrils& outputs)
    {
      couch::Document doc = docs[current_frame];
      doc.update();
      obs << doc; //read the observation from the doc.
      obs >> outputs; //push the observation to the outputs.
      current_frame++;
      if(current_frame >= int( docs.size())) return ecto::QUIT;
      return 0;
    }
    std::vector<couch::Document> docs;
    Observation obs;
    int total_rows, offset;
    couch::Db db;
    int current_frame;
  };

}

BOOST_PYTHON_MODULE(objcog_db)
{
  ecto::wrap<db::ObservationInserter>("ObservationInserter");
  ecto::wrap<db::ObservationReader>("ObservationReader");

}
;
