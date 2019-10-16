package org.greenplum.pxf.plugins.hdfs;

/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */


import org.apache.avro.Schema;
import org.apache.avro.file.DataFileWriter;
import org.apache.avro.generic.GenericDatumWriter;
import org.apache.avro.generic.GenericRecord;
import org.apache.avro.io.DatumWriter;
import org.apache.avro.mapred.AvroInputFormat;
import org.apache.avro.mapred.AvroJob;
import org.apache.avro.mapred.AvroRecordReader;
import org.apache.avro.mapred.AvroWrapper;
import org.apache.hadoop.fs.FSDataOutputStream;
import org.apache.hadoop.fs.FileSystem;
import org.apache.hadoop.fs.Path;
import org.apache.hadoop.io.NullWritable;
import org.apache.hadoop.mapred.FileSplit;
import org.apache.hadoop.mapred.InputSplit;
import org.apache.hadoop.mapred.JobConf;
import org.greenplum.pxf.api.OneRow;
import org.greenplum.pxf.api.model.RequestContext;
import org.greenplum.pxf.plugins.hdfs.utilities.AvroUtilities;

import java.io.IOException;

/**
 * A PXF Accessor for reading Avro File records
 */
public class AvroFileAccessor extends HdfsSplittableDataAccessor {

    private AvroWrapper<GenericRecord> avroWrapper;
    private DataFileWriter<GenericRecord> writer;
    private long rowsWritten, totalRowsWritten;
    private Schema schema;

    /**
     * Constructs a new instance of the AvroFileAccessor
     */
    public AvroFileAccessor() {
        super(new AvroInputFormat<GenericRecord>());
    }

    /*
     * Initializes an AvroFileAccessor.
     *
     * We need schema to be read or generated before
     * AvroResolver#initialize() is called so that
     * AvroResolver#fields can be set.
     *
     * for READ:
     * creates the job configuration and accesses the data
     * source avro file or a user-provided path to an avro file
     * to fetch the avro schema.
     *
     * for WRITE:
     * We get the schema either from a user-provided path to an
     * avro file or by generating it on the fly.
     */
    @Override
    public void initialize(RequestContext requestContext) {
        super.initialize(requestContext);

        schema = AvroUtilities.getInstance().obtainSchema(context, configuration);

    }

    @Override
    public boolean openForRead() throws Exception {
        boolean ret = super.openForRead();
        // Pass the schema to the AvroInputFormat
        AvroJob.setInputSchema(jobConf, schema);

        // The avroWrapper required for the iteration
        avroWrapper = new AvroWrapper<>();

        return ret;
    }

    @Override
    protected Object getReader(JobConf jobConf, InputSplit split) throws IOException {
        return new AvroRecordReader<>(jobConf, (FileSplit) split);
    }

    /**
     * readNextObject
     * The AVRO accessor is currently the only specialized accessor that
     * overrides this method. This happens, because the special
     * AvroRecordReader.next() semantics (use of the AvroWrapper), so it
     * cannot use the RecordReader's default implementation in
     * SplittableFileAccessor
     */
    @Override
    public OneRow readNextObject() throws IOException {
        /** Resetting datum to null, to avoid stale bytes to be padded from the previous row's datum */
        avroWrapper.datum(null);
        if (reader.next(avroWrapper, NullWritable.get())) { // There is one more record in the current split.
            return new OneRow(null, avroWrapper.datum());
        } else if (getNextSplit()) { // The current split is exhausted. try to move to the next split.
            return reader.next(avroWrapper, NullWritable.get())
                    ? new OneRow(null, avroWrapper.datum())
                    : null;
        }

        // if neither condition was met, it means we already read all the records in all the splits, and
        // in this call record variable was not set, so we return null and thus we are signaling end of
        // records sequence - in this case avroWrapper.datum() will be null
        return null;
    }

    /**
     * Opens the resource for write.
     *
     * @return true if the resource is successfully opened
     * @throws Exception if opening the resource failed
     */
    @Override
    public boolean openForWrite() throws Exception {
        // make writer
        DatumWriter<GenericRecord> datumWriter = new GenericDatumWriter<>(schema);
        writer = new DataFileWriter<>(datumWriter);
        Path file = new Path(hcfsType.getUriForWrite(configuration, context, true) + ".avro");
        FileSystem fs = file.getFileSystem(jobConf);
        FSDataOutputStream avroOut = fs.create(file, false);
        writer.create(schema, avroOut);

        return true;
    }


    /**
     * Writes the next object.
     *
     * @param onerow the object to be written
     * @return true if the write succeeded
     * @throws Exception writing to the resource failed
     */
    @Override
    public boolean writeNextObject(OneRow onerow) throws Exception {
        writer.append((GenericRecord) onerow.getData());
        rowsWritten++;
        return true;
    }

    /**
     * Closes the resource for write.
     *
     * @throws Exception if closing the resource failed
     */
    @Override
    public void closeForWrite() throws Exception {
        if (writer != null) {
            writer.close();
            totalRowsWritten += rowsWritten;
        }
        LOG.debug("Wrote a TOTAL of {} rows", totalRowsWritten);
    }
}
